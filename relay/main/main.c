/**
 * main.c — Flower ESP32 relay controller (ESP-IDF)
 *
 * Features:
 *   1. WiFi STA + NTP time sync
 *   2. HMAC-SHA256 MQTT credentials (clientId{id}timestamp{ms})
 *   3. MQTT 5.0 with User Property {"project","flower"} on CONNECT
 *   4. nanopb: receives relay_control commands; sends status_report + cmd_response
 *   5. GPIO: relay (pin 4, active-low), button (pin 9), breathing LED (pin 3)
 *   6. HTTP server: GET /change_relay1[?duration=ms]
 *
 * Topics:
 *   sub  flower/{DEVICE_ID}/down/command
 *   pub  flower/{DEVICE_ID}/up/status
 *   pub  flower/{DEVICE_ID}/up/command_response
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"

#include "esp_http_server.h"

#include "mqtt_client.h"
#include "mqtt5_client.h"

#include "psa/crypto.h"

#include "pb_encode.h"
#include "pb_decode.h"
#include "proto/device.pb.h"

/* ── Config ──────────────────────────────────────────────────────────────── */
#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID      "your_ssid"
#endif
#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD  "your_password"
#endif
#ifndef CONFIG_MQTT_BROKER_URI
#define CONFIG_MQTT_BROKER_URI "mqtts://your-emqx-host:8883"
#endif

/* Device identity — loaded at runtime from fctry NVS partition */
static char g_device_id[64];
static char g_device_secret[128];

/* MQTT topics — built at runtime after device identity is loaded */
static char s_topic_cmd[96];
static char s_topic_cmd_resp[96];
static char s_topic_status[96];

/* ── Hardware ────────────────────────────────────────────────────────────── */
#define BUTTON_PIN  9
#define RELAY_PIN   4
#define LED_PIN     3
#define ADC_PIN     ADC_CHANNEL_0   /* GPIO0 = ADC1 CH0 */

#define STATUS_INTERVAL_MS        30000
#define RELAY_DEFAULT_DURATION_MS 10000
#define BREATH_PERIOD_MS          3000
#define APP_TIMER_INTERVAL_MS     50   /* LED + button + relay auto-off tick */

#define TAG "Flower"

/* ── Device identity ─────────────────────────────────────────────────────── */
static void device_identity_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition("fctry", "identity", NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fctry partition open failed (%s), identity unavailable",
                 esp_err_to_name(err));
        goto build_topics;
    }
    size_t len = sizeof(g_device_id);
    nvs_get_str(h, "device_id",     g_device_id,     &len);
    len = sizeof(g_device_secret);
    nvs_get_str(h, "device_secret", g_device_secret, &len);
    nvs_close(h);
    ESP_LOGI(TAG, "Device identity loaded from fctry: id=%s", g_device_id);

build_topics:
    snprintf(s_topic_cmd,      sizeof(s_topic_cmd),      "flower/%s/down/cmd",        g_device_id);
    snprintf(s_topic_cmd_resp, sizeof(s_topic_cmd_resp), "flower/%s/up/cmd_response", g_device_id);
    snprintf(s_topic_status,   sizeof(s_topic_status),   "flower/%s/up/status",       g_device_id);
}

/* ── State ───────────────────────────────────────────────────────────────── */
static EventGroupHandle_t       s_wifi_event_group;
#define WIFI_CONNECTED_BIT      BIT0

static esp_mqtt_client_handle_t s_mqtt_client    = NULL;
static volatile bool            s_mqtt_connected = false;

static volatile bool            s_relay_on       = false;
static volatile int64_t         s_relay_on_us    = 0;   /* esp_timer_get_time() at relay-on */
static volatile uint32_t        s_relay_duration = RELAY_DEFAULT_DURATION_MS;

static volatile bool            s_last_btn       = true; /* HIGH = not pressed */

/* Static to avoid large stack allocation (device_command_response_t ≈ 6 KB) */
static device_command_t          s_cmd;
static device_command_response_t s_resp;

/* ── ADC ─────────────────────────────────────────────────────────────────── */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;

static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,   /* 0 ~ 3.3 V */
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, ADC_PIN, &chan_cfg));
}

static int adc_read_raw(void)
{
    int raw = 0;
    adc_oneshot_read(s_adc_handle, ADC_PIN, &raw);
    return raw;
}

/* ── HMAC-SHA256 ─────────────────────────────────────────────────────────── */
static void calc_signature(int64_t ts_ms, const char *device_id,
                            const char *secret, char out[65])
{
    char plaintext[128];
    snprintf(plaintext, sizeof(plaintext),
             "clientId%stimestamp%lld", device_id, (long long)ts_ms);

    /* HMAC-SHA256 via PSA Crypto API (ESP-IDF v6 / mbedTLS 4.x) */
    uint8_t hmac[32];
    size_t hmac_len;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    psa_import_key(&attr, (const uint8_t *)secret, strlen(secret), &key_id);
    psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                    (const uint8_t *)plaintext, strlen(plaintext),
                    hmac, sizeof(hmac), &hmac_len);
    psa_destroy_key(key_id);

    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", hmac[i]);
    out[64] = '\0';
}

/* ── nanopb publish helpers ──────────────────────────────────────────────── */
static void publish_cmd_resp(void)
{
    uint8_t buf[128];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, &device_command_response_t_msg, &s_resp)) {
        ESP_LOGE(TAG, "resp encode: %s", PB_GET_ERROR(&stream));
        return;
    }
    esp_mqtt_client_publish(s_mqtt_client, s_topic_cmd_resp,
                            (const char *)buf, (int)stream.bytes_written, 1, 0);
}

static void send_relay_resp(bool success)
{
    memset(&s_resp, 0, sizeof(s_resp));
    strncpy(s_resp.device_id, g_device_id, sizeof(s_resp.device_id) - 1);
    s_resp.which_payload = DEVICE_COMMAND_RESPONSE_RELAY_CONTROL_TAG;
    s_resp.payload.relay_control.result =
        success ? DEVICE_CMD_RESULT_CMD_RESULT_OK
                : DEVICE_CMD_RESULT_CMD_RESULT_BUSY;
    publish_cmd_resp();
}

static void send_unsupported_resp(pb_size_t which_cmd)
{
    memset(&s_resp, 0, sizeof(s_resp));
    strncpy(s_resp.device_id, g_device_id, sizeof(s_resp.device_id) - 1);

    switch (which_cmd) {
    case DEVICE_COMMAND_JOIN_ROOM_TAG:
        s_resp.which_payload = DEVICE_COMMAND_RESPONSE_JOIN_ROOM_TAG;
        s_resp.payload.join_room.result = DEVICE_CMD_RESULT_CMD_RESULT_BUSY;
        break;
    case DEVICE_COMMAND_LEAVE_ROOM_TAG:
        s_resp.which_payload = DEVICE_COMMAND_RESPONSE_LEAVE_ROOM_TAG;
        s_resp.payload.leave_room.result = DEVICE_CMD_RESULT_CMD_RESULT_BUSY;
        break;
    case DEVICE_COMMAND_UPLOAD_LOGS_TAG:
        s_resp.which_payload = DEVICE_COMMAND_RESPONSE_UPLOAD_LOGS_TAG;
        s_resp.payload.upload_logs.result = DEVICE_CMD_RESULT_CMD_RESULT_BUSY;
        break;
    case DEVICE_COMMAND_OPEN_LIGHT_TAG:
        s_resp.which_payload = DEVICE_COMMAND_RESPONSE_OPEN_LIGHT_TAG;
        s_resp.payload.open_light.result = DEVICE_CMD_RESULT_CMD_RESULT_BUSY;
        break;
    case DEVICE_COMMAND_CLOSE_LIGHT_TAG:
        s_resp.which_payload = DEVICE_COMMAND_RESPONSE_CLOSE_LIGHT_TAG;
        s_resp.payload.close_light.result = DEVICE_CMD_RESULT_CMD_RESULT_BUSY;
        break;
    case DEVICE_COMMAND_TAKE_PHOTO_TAG:
        s_resp.which_payload = DEVICE_COMMAND_RESPONSE_TAKE_PHOTO_TAG;
        s_resp.payload.take_photo.result = DEVICE_CMD_RESULT_CMD_RESULT_BUSY;
        break;
    case DEVICE_COMMAND_REBOOT_TAG:
        s_resp.which_payload = DEVICE_COMMAND_RESPONSE_REBOOT_TAG;
        s_resp.payload.reboot.result = DEVICE_CMD_RESULT_CMD_RESULT_BUSY;
        break;
    default:
        ESP_LOGW(TAG, "Unknown cmd tag=%d, no response", (int)which_cmd);
        return;
    }
    ESP_LOGW(TAG, "Unsupported cmd tag=%d, sending BUSY", (int)which_cmd);
    publish_cmd_resp();
}

/* ── MQTT message handler ────────────────────────────────────────────────── */
static void handle_mqtt_data(const char *data, int data_len)
{
    memset(&s_cmd, 0, sizeof(s_cmd));
    pb_istream_t stream = pb_istream_from_buffer((const pb_byte_t *)data, data_len);
    if (!pb_decode(&stream, &device_command_t_msg, &s_cmd)) {
        ESP_LOGE(TAG, "Command decode: %s", PB_GET_ERROR(&stream));
        return;
    }

    if (s_cmd.which_payload != DEVICE_COMMAND_RELAY_CONTROL_TAG) {
        send_unsupported_resp(s_cmd.which_payload);
        return;
    }

    device_relay_control_t *rc = &s_cmd.payload.relay_control;
    if (rc->on) {
        if (s_relay_on) {
            ESP_LOGW(TAG, "Relay already ON, ignoring command");
            send_relay_resp(false);
            return;
        }
        gpio_set_level(RELAY_PIN, 0);
        s_relay_on_us = esp_timer_get_time();
        if (rc->duration_ms > 0) {
            s_relay_duration = rc->duration_ms;
            s_relay_on = true;
        } else {
            s_relay_on = false; /* duration_ms=0: no auto-off */
        }
    } else {
        gpio_set_level(RELAY_PIN, 1);
        s_relay_on = false;
    }

    send_relay_resp(true);
    ESP_LOGI(TAG, "Relay %s via MQTT (duration=%u ms)",
             rc->on ? "ON" : "OFF", (unsigned)rc->duration_ms);
}

static void publish_status_report(void)
{
    int raw = adc_read_raw();
    ESP_LOGI(TAG, "ADC IO0 raw=%d", raw);

    if (!s_mqtt_connected) return;

    time_t now; time(&now);

    device_status_report_t sr = DEVICE_STATUS_REPORT_INIT_ZERO;
    sr.signal_dbm = (int32_t)raw;
    sr.timestamp  = (int64_t)now * 1000;

    uint8_t buf[DEVICE_STATUS_REPORT_SIZE];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, &device_status_report_t_msg, &sr)) {
        ESP_LOGE(TAG, "Status encode: %s", PB_GET_ERROR(&stream));
        return;
    }
    esp_mqtt_client_publish(s_mqtt_client, s_topic_status,
                            (const char *)buf, (int)stream.bytes_written, 1, 0);
    ESP_LOGI(TAG, "Status published (signal_dbm=%d)", raw);
}

/* ── MQTT ────────────────────────────────────────────────────────────────── */
static esp_timer_handle_t s_reconnect_timer = NULL;
static esp_timer_handle_t s_status_timer    = NULL;

static void mqtt_start(void);

/* Runs in ESP timer task — safe to call esp_mqtt_client_destroy here */
static void reconnect_timer_cb(void *arg)
{
    if (s_mqtt_client) {
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
    }
    mqtt_start();
}

static void status_timer_cb(void *arg)
{
    publish_status_report();
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected = true;
        esp_mqtt_client_subscribe(s_mqtt_client, s_topic_cmd, 1);
        ESP_LOGI(TAG, "MQTT connected, sub: %s", s_topic_cmd);
        publish_status_report();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected, reconnect in 5 s");
        /* Don't destroy here — we're inside the MQTT task.
           Timer task will do it safely. */
        esp_timer_start_once(s_reconnect_timer, 5000000ULL);
        break;
    case MQTT_EVENT_DATA:
        handle_mqtt_data(event->data, event->data_len);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error type=%d", event->error_handle->error_type);
        break;
    default:
        break;
    }
}

static char s_mqtt_username[80];
static char s_mqtt_password[65];

static void mqtt_start(void)
{
    time_t now; time(&now);
    int64_t ts_ms = (int64_t)now * 1000;
    snprintf(s_mqtt_username, sizeof(s_mqtt_username),
             "%s|%lld", g_device_id, (long long)ts_ms);
    calc_signature(ts_ms, g_device_id, g_device_secret, s_mqtt_password);

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri                            = CONFIG_MQTT_BROKER_URI;
    mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
    mqtt_cfg.credentials.client_id                         = g_device_id;
    mqtt_cfg.credentials.username                          = s_mqtt_username;
    mqtt_cfg.credentials.authentication.password           = s_mqtt_password;
    mqtt_cfg.session.keepalive                             = 60;
    mqtt_cfg.session.protocol_ver                          = MQTT_PROTOCOL_V_5;
    mqtt_cfg.network.disable_auto_reconnect                = true;

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

    /* MQTT 5.0 User Property: self-identify as "flower" project */
    esp_mqtt5_user_property_item_t prop_items[] = {{"project", "flower"}};
    mqtt5_user_property_handle_t prop_handle = NULL;
    esp_mqtt5_client_set_user_property(&prop_handle, prop_items, 1);
    esp_mqtt5_connection_property_config_t connect_prop = {};
    connect_prop.user_property = prop_handle;
    esp_mqtt5_client_set_connect_property(s_mqtt_client, &connect_prop);
    esp_mqtt5_client_delete_user_property(prop_handle);

    esp_mqtt_client_register_event(s_mqtt_client, MQTT_EVENT_ANY,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
}

/* ── HTTP server ─────────────────────────────────────────────────────────── */
static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_send(req, "1", 1);
    return ESP_OK;
}

static esp_err_t handle_change_relay1(httpd_req_t *req)
{
    char query[64] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16] = {};
        if (httpd_query_key_value(query, "duration", val, sizeof(val)) == ESP_OK) {
            uint32_t d = (uint32_t)atoi(val);
            if (d > 0) s_relay_duration = d;
        }
    }
    gpio_set_level(RELAY_PIN, 0);
    s_relay_on_us = esp_timer_get_time();
    s_relay_on    = true;
    httpd_resp_send(req, "0", 1);
    return ESP_OK;
}

static void http_server_start(void)
{
    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server  = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return;
    }
    static const httpd_uri_t uri_root = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = handle_root,
    };
    static const httpd_uri_t uri_relay = {
        .uri     = "/change_relay1",
        .method  = HTTP_GET,
        .handler = handle_change_relay1,
    };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_relay);
    ESP_LOGI(TAG, "HTTP server started on port 80");
}

/* ── App timer: LED breath, button poll, relay auto-off ──────────────────── */
static esp_timer_handle_t s_app_timer = NULL;
static int64_t            s_app_tick  = 0;

#define BREATH_STEPS (BREATH_PERIOD_MS / APP_TIMER_INTERVAL_MS)  /* 60 steps */

static void app_timer_cb(void *arg)
{
    s_app_tick++;

    /* Breathing LED */
    float phase = (float)(s_app_tick % BREATH_STEPS) / (float)BREATH_STEPS
                  * 2.0f * (float)M_PI;
    uint32_t duty = (uint32_t)(((sinf(phase - (float)M_PI / 2.0f) + 1.0f) / 2.0f) * 255.0f);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

    /* Button: active-low, toggle relay on falling edge */
    bool btn = (bool)gpio_get_level(BUTTON_PIN);
    if (!btn && s_last_btn)
        gpio_set_level(RELAY_PIN, !gpio_get_level(RELAY_PIN));
    s_last_btn = btn;

    /* Relay auto-off */
    if (s_relay_on) {
        int64_t elapsed_ms = (esp_timer_get_time() - s_relay_on_us) / 1000LL;
        if (elapsed_ms >= (int64_t)s_relay_duration) {
            gpio_set_level(RELAY_PIN, 1);
            s_relay_on = false;
        }
    }
}

/* ── WiFi ────────────────────────────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &h2));

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid,     CONFIG_WIFI_SSID,    sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for WiFi...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */
void app_main(void)
{
    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* PSA Crypto — must be called before any psa_* API */
    ESP_ERROR_CHECK(psa_crypto_init());

    /* Factory partition — device identity (id + secret) */
    nvs_flash_init_partition("fctry");
    device_identity_init();

    /* ADC: IO0 */
    adc_init();

    /* GPIO: relay (output, default off) */
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << RELAY_PIN);
    io_conf.mode         = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(RELAY_PIN, 1);

    /* GPIO: button (input pull-up) */
    io_conf.pin_bit_mask = (1ULL << BUTTON_PIN);
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /* LEDC: breathing LED */
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_8_BIT,
        .freq_hz          = 1000,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_ch = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = LED_PIN,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_ch));

    /* App timer: 50 ms — LED breath, button, relay auto-off */
    esp_timer_create_args_t app_args = {
        .callback              = app_timer_cb,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "app",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&app_args, &s_app_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_app_timer,
                        (uint64_t)APP_TIMER_INTERVAL_MS * 1000ULL));

    /* Status timer: 30 s — ADC read + status report */
    esp_timer_create_args_t st_args = {
        .callback              = status_timer_cb,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "status",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&st_args, &s_status_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_status_timer,
                        (uint64_t)STATUS_INTERVAL_MS * 1000ULL));

    /* Reconnect timer: one-shot, started on MQTT_EVENT_DISCONNECTED */
    esp_timer_create_args_t rc_args = {
        .callback        = reconnect_timer_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&rc_args, &s_reconnect_timer));

    /* WiFi */
    wifi_init_sta();

    /* NTP (UTC+8) — needed for HMAC timestamp */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_init();

    ESP_LOGI(TAG, "Waiting for NTP sync...");
    time_t now = 0;
    for (int i = 0; i < 40 && now < 1700000000LL; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        time(&now);
    }
    ESP_LOGI(TAG, "NTP %s (ts=%lld)", now > 1700000000LL ? "OK" : "FAILED",
             (long long)now);

    /* MQTT */
    mqtt_start();

    /* HTTP */
    http_server_start();

    /* Idle — all work is event/timer-driven */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "Uptime: %lld s", esp_timer_get_time() / 1000000LL);
    }
}
