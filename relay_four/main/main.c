/**
 * main.c — Flower ESP32-C3 4-channel relay controller (ESP-IDF)
 *
 * Features:
 *   1. Hardware mapping:
 *      - Relay 1: GPIO 3
 *      - Relay 2: GPIO 4
 *      - Relay 3: GPIO 5
 *      - Relay 4: GPIO 7
 *      - Button 1: GPIO 10 (controls Relay 1)
 *      - Button 2: GPIO 9  (controls Relay 2)
 *      - Button 3: GPIO 6  (controls Relay 3)
 *      - Button 4: GPIO 8  (controls Relay 4)
 *      - Status LED: GPIO 2 (breathing LED via LEDC)
 *   2. WiFi STA + SNTP time sync (ntp.aliyun.com & pool.ntp.org)
 *   3. HMAC-SHA256 MQTT credentials (clientId{id}timestamp{ms}) via PSA Crypto
 *   4. MQTT 5.0 with User Property {"project","flower"}
 *   5. nanopb: receives relay_control (channel 0) & ota commands;
 *              sends status_report (12 metrics for 4 channels) + cmd_response
 *   6. HTTP server:
 *      - GET / -> 4-channel status web console
 *      - GET /change_relay1, /change_relay2, /change_relay3, /change_relay4
 *      - GET /api/sensors -> JSON with 4-channel states and timestamps
 *   7. OTA: HTTPS firmware update triggered via MQTT OtaCommand
 *
 * Topics:
 *   sub  flower/{DEVICE_ID}/down/cmd
 *   pub  flower/{DEVICE_ID}/up/status
 *   pub  flower/{DEVICE_ID}/up/cmd_response
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/ledc.h"

#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"

#include "mqtt_client.h"
#include "mqtt5_client.h"

#include "psa/crypto.h"

#include "pb_encode.h"
#include "pb_decode.h"
#include "flower.pb.h"

/* ── Config ──────────────────────────────────────────────────────────────── */
#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID      "CU_home"
#endif
#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD  "wastemytimehome123456789"
#endif
#ifndef CONFIG_MQTT_BROKER_URI
#define CONFIG_MQTT_BROKER_URI "mqtts://58.87.80.78:8883"
#endif

/* Device identity — loaded at runtime from fctry NVS partition */
static char g_device_id[64];
static char g_device_secret[128];

/* MQTT topics — built at runtime after device identity is loaded */
static char s_topic_cmd[96];
static char s_topic_cmd_resp[96];
static char s_topic_status[96];
static char s_host_name[96];

/* ── Hardware & Channels ─────────────────────────────────────────────────── */
#define LED_PIN                   GPIO_NUM_2
#define STATUS_INTERVAL_MS        30000
#define RELAY_DEFAULT_DURATION_MS 10000
#define BREATH_PERIOD_MS          3000
#define APP_TIMER_INTERVAL_MS     50   /* LED breath + button poll + relay auto-off */

#define TAG "FlowerRelayFour"

typedef struct {
    gpio_num_t       relay_gpio;
    gpio_num_t       button_gpio;
    volatile bool    on;
    volatile int64_t on_us;
    volatile uint32_t duration_ms;
    volatile int64_t last_on_utc_s;
    volatile uint32_t last_on_dur_ms;
    bool             last_button_state;
    uint8_t          btn_debounce_cnt;
} relay_channel_t;

static SemaphoreHandle_t s_relay_mutex  = NULL;
static SemaphoreHandle_t s_status_mutex = NULL;

static relay_channel_t s_relays[4] = {
    { .relay_gpio = GPIO_NUM_3, .button_gpio = GPIO_NUM_10, .on = false, .duration_ms = RELAY_DEFAULT_DURATION_MS, .last_button_state = true, .btn_debounce_cnt = 0 },
    { .relay_gpio = GPIO_NUM_4, .button_gpio = GPIO_NUM_9,  .on = false, .duration_ms = RELAY_DEFAULT_DURATION_MS, .last_button_state = true, .btn_debounce_cnt = 0 },
    { .relay_gpio = GPIO_NUM_5, .button_gpio = GPIO_NUM_6,  .on = false, .duration_ms = RELAY_DEFAULT_DURATION_MS, .last_button_state = true, .btn_debounce_cnt = 0 },
    { .relay_gpio = GPIO_NUM_7, .button_gpio = GPIO_NUM_8,  .on = false, .duration_ms = RELAY_DEFAULT_DURATION_MS, .last_button_state = true, .btn_debounce_cnt = 0 },
};

static void relay_set_unlocked(int ch, bool on, uint32_t duration_ms)
{
    if (ch < 0 || ch >= 4) return;
    if (on) {
        s_relays[ch].on_us = esp_timer_get_time();
        s_relays[ch].duration_ms = duration_ms;
        time_t _now;
        time(&_now);
        s_relays[ch].last_on_utc_s = (int64_t)_now;
        s_relays[ch].last_on_dur_ms = duration_ms;
    }
    s_relays[ch].on = on;
    gpio_set_level(s_relays[ch].relay_gpio, on ? 1 : 0);
}

void relay_set(int ch, bool on, uint32_t duration_ms)
{
    if (ch < 0 || ch >= 4) return;
    if (s_relay_mutex) xSemaphoreTake(s_relay_mutex, portMAX_DELAY);
    if (on) {
        s_relays[ch].on_us = esp_timer_get_time();
        s_relays[ch].duration_ms = duration_ms;
        time_t _now;
        time(&_now);
        s_relays[ch].last_on_utc_s = (int64_t)_now;
        s_relays[ch].last_on_dur_ms = duration_ms;
    }
    s_relays[ch].on = on;
    gpio_set_level(s_relays[ch].relay_gpio, on ? 1 : 0);
    if (s_relay_mutex) xSemaphoreGive(s_relay_mutex);
}

static void relay_toggle_unlocked(int ch)
{
    if (ch < 0 || ch >= 4) return;
    if (s_relays[ch].on) {
        relay_set_unlocked(ch, false, 0);
    } else {
        relay_set_unlocked(ch, true, s_relays[ch].duration_ms > 0 ? s_relays[ch].duration_ms : RELAY_DEFAULT_DURATION_MS);
    }
}

void relay_toggle(int ch)
{
    if (ch < 0 || ch >= 4) return;
    if (s_relay_mutex) xSemaphoreTake(s_relay_mutex, portMAX_DELAY);
    relay_toggle_unlocked(ch);
    if (s_relay_mutex) xSemaphoreGive(s_relay_mutex);
}

/* ── Device identity ─────────────────────────────────────────────────────── */
static esp_err_t device_identity_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition("fctry", "identity", NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fctry partition open failed (%s), identity unavailable",
                 esp_err_to_name(err));
        return err;
    }
    size_t len = sizeof(g_device_id);
    err = nvs_get_str(h, "device_id", g_device_id, &len);
    if (err != ESP_OK || g_device_id[0] == '\0') {
        ESP_LOGE(TAG, "Failed to read valid device_id from fctry (%s)", esp_err_to_name(err));
        nvs_close(h);
        return (err != ESP_OK) ? err : ESP_ERR_NOT_FOUND;
    }
    len = sizeof(g_device_secret);
    err = nvs_get_str(h, "device_secret", g_device_secret, &len);
    if (err != ESP_OK || g_device_secret[0] == '\0') {
        ESP_LOGE(TAG, "Failed to read valid device_secret from fctry (%s)", esp_err_to_name(err));
        nvs_close(h);
        return (err != ESP_OK) ? err : ESP_ERR_NOT_FOUND;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Device identity loaded from fctry: id=%s", g_device_id);

    snprintf(s_topic_cmd,      sizeof(s_topic_cmd),      "flower/%s/down/cmd",        g_device_id);
    snprintf(s_topic_cmd_resp, sizeof(s_topic_cmd_resp), "flower/%s/up/cmd_response", g_device_id);
    snprintf(s_topic_status,   sizeof(s_topic_status),   "flower/%s/up/status",       g_device_id);

    snprintf(s_host_name,      sizeof(s_host_name),      "rel_%s",                    g_device_id);
    return ESP_OK;
}

/* ── State ───────────────────────────────────────────────────────────────── */
static EventGroupHandle_t       s_wifi_event_group;
#define WIFI_CONNECTED_BIT      BIT0

static esp_mqtt_client_handle_t s_mqtt_client    = NULL;
static volatile bool            s_mqtt_connected = false;

/* Static to avoid large stack allocation */
static flower_command_t          s_cmd;
static flower_command_response_t s_resp;

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
static void publish_resp(const flower_command_response_t *resp)
{
    uint8_t buf[128];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, &flower_command_response_t_msg, resp)) {
        ESP_LOGE(TAG, "resp encode: %s", PB_GET_ERROR(&stream));
        return;
    }
    esp_mqtt_client_publish(s_mqtt_client, s_topic_cmd_resp,
                            (const char *)buf, (int)stream.bytes_written, 1, 0);
}

static void send_relay_resp(bool success)
{
    memset(&s_resp, 0, sizeof(s_resp));
    s_resp.which_payload = FLOWER_COMMAND_RESPONSE_RELAY_CONTROL_TAG;
    s_resp.payload.relay_control.result =
        success ? FLOWER_CMD_RESULT_OK : FLOWER_CMD_RESULT_BUSY;
    publish_resp(&s_resp);
}

/* ── OTA ─────────────────────────────────────────────────────────────────── */
#define OTA_URL_MAX    256
#define OTA_TASK_STACK 8192

static QueueHandle_t  s_ota_url_queue   = NULL;
static volatile bool  s_ota_in_progress = false;

static void publish_ota_status(flower_ota_status_t status, int32_t progress,
                               const char *msg)
{
    flower_command_response_t resp = FLOWER_COMMAND_RESPONSE_INIT_ZERO;
    resp.which_payload             = FLOWER_COMMAND_RESPONSE_OTA_TAG;
    resp.payload.ota.status        = status;
    resp.payload.ota.progress      = progress;
    if (msg)
        strncpy(resp.payload.ota.message, msg, sizeof(resp.payload.ota.message) - 1);
    publish_resp(&resp);
}

static void ota_task(void *arg)
{
    char url[OTA_URL_MAX];
    for (;;) {
        xQueueReceive(s_ota_url_queue, url, portMAX_DELAY);
        s_ota_in_progress = true;

        ESP_LOGI(TAG, "OTA starting: %s", url);
        publish_ota_status(FLOWER_OTA_STATUS_OTA_STARTED, 0, NULL);

        esp_http_client_config_t http_cfg = {
            .url                        = url,
            .timeout_ms                 = 30000,
            .keep_alive_enable          = true,
            .skip_cert_common_name_check = true,
        };
        esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

        esp_https_ota_handle_t handle = NULL;
        esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA begin: %s", esp_err_to_name(err));
            publish_ota_status(FLOWER_OTA_STATUS_OTA_FAILED, 0, esp_err_to_name(err));
            s_ota_in_progress = false;
            continue;
        }

        int img_size   = esp_https_ota_get_image_size(handle);
        int last_pct10 = -1;

        while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            if (img_size > 0) {
                int read = esp_https_ota_get_image_len_read(handle);
                int pct  = (int)((int64_t)read * 100 / img_size);
                int bucket = pct / 10;
                if (bucket != last_pct10) {
                    last_pct10 = bucket;
                    publish_ota_status(FLOWER_OTA_STATUS_OTA_PROGRESS, pct, NULL);
                }
            }
        }

        if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
            ESP_LOGE(TAG, "OTA download: %s", esp_err_to_name(err));
            esp_https_ota_abort(handle);
            publish_ota_status(FLOWER_OTA_STATUS_OTA_FAILED, 0, esp_err_to_name(err));
            s_ota_in_progress = false;
            continue;
        }

        err = esp_https_ota_finish(handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA finish: %s", esp_err_to_name(err));
            publish_ota_status(FLOWER_OTA_STATUS_OTA_FAILED, 0, esp_err_to_name(err));
            s_ota_in_progress = false;
            continue;
        }

        publish_ota_status(FLOWER_OTA_STATUS_OTA_SUCCESS, 100, NULL);
        ESP_LOGI(TAG, "OTA done, restarting in 2 s");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
}

/* ── MQTT message handler ────────────────────────────────────────────────── */
static void handle_mqtt_data(const char *data, int data_len)
{
    memset(&s_cmd, 0, sizeof(s_cmd));
    pb_istream_t stream = pb_istream_from_buffer((const pb_byte_t *)data, data_len);
    if (!pb_decode(&stream, &flower_command_t_msg, &s_cmd)) {
        ESP_LOGE(TAG, "Command decode: %s", PB_GET_ERROR(&stream));
        return;
    }

    switch (s_cmd.which_payload) {
    case FLOWER_COMMAND_RELAY_CONTROL_TAG: {
        flower_relay_control_t *rc = &s_cmd.payload.relay_control;
        if (rc->on) {
            relay_set(0, true, rc->duration_ms > 0 ? rc->duration_ms : RELAY_DEFAULT_DURATION_MS);
        } else {
            relay_set(0, false, 0);
        }
        send_relay_resp(true);
        ESP_LOGI(TAG, "Relay 1 %s via MQTT (duration=%u ms)",
                 rc->on ? "ON" : "OFF", (unsigned)rc->duration_ms);
        break;
    }
    case FLOWER_COMMAND_OTA_TAG: {
        if (s_ota_in_progress) {
            ESP_LOGW(TAG, "OTA already in progress, ignoring");
            break;
        }
        if (s_cmd.payload.ota.url[0] == '\0') {
            ESP_LOGE(TAG, "OTA: empty URL");
            break;
        }
        if (xQueueSend(s_ota_url_queue, s_cmd.payload.ota.url, 0) != pdTRUE)
            ESP_LOGE(TAG, "OTA queue full");
        break;
    }
    default:
        ESP_LOGW(TAG, "Unknown cmd tag=%d", (int)s_cmd.which_payload);
        break;
    }
}

/* ── Status Report ───────────────────────────────────────────────────────── */
static flower_metric_t s_metrics[12];

static bool encode_metrics(pb_ostream_t *stream, const pb_field_t *field, void *const *arg)
{
    flower_metric_t *metrics = (flower_metric_t *)(*arg);
    for (int i = 0; i < 12; i++) {
        if (!pb_encode_tag_for_field(stream, field))
            return false;
        if (!pb_encode_submessage(stream, FLOWER_METRIC_FIELDS, &metrics[i]))
            return false;
    }
    return true;
}

static void publish_status_report(void)
{
    if (s_status_mutex) xSemaphoreTake(s_status_mutex, portMAX_DELAY);

    if (!s_mqtt_connected || !s_mqtt_client) {
        if (s_status_mutex) xSemaphoreGive(s_status_mutex);
        return;
    }

    /* 12 metrics:
     *   relay1State ... relay4State (INT64)
     *   relay1LastOnTs ... relay4LastOnTs (INT64)
     *   relay1LastOnDur ... relay4LastOnDur (INT64)
     */
    if (s_relay_mutex) xSemaphoreTake(s_relay_mutex, portMAX_DELAY);
    for (int i = 0; i < 4; i++) {
        snprintf(s_metrics[i].key, sizeof(s_metrics[i].key), "relay%dState", i + 1);
        s_metrics[i].which_value = FLOWER_METRIC_INT64_VALUE_TAG;
        s_metrics[i].value.int64_value = s_relays[i].on ? 1 : 0;
    }
    for (int i = 0; i < 4; i++) {
        snprintf(s_metrics[4 + i].key, sizeof(s_metrics[4 + i].key), "relay%dLastOnTs", i + 1);
        s_metrics[4 + i].which_value = FLOWER_METRIC_INT64_VALUE_TAG;
        s_metrics[4 + i].value.int64_value = s_relays[i].last_on_utc_s;
    }
    for (int i = 0; i < 4; i++) {
        snprintf(s_metrics[8 + i].key, sizeof(s_metrics[8 + i].key), "relay%dLastOnDur", i + 1);
        s_metrics[8 + i].which_value = FLOWER_METRIC_INT64_VALUE_TAG;
        s_metrics[8 + i].value.int64_value = (int64_t)s_relays[i].last_on_dur_ms;
    }
    int r_on[4];
    for (int i = 0; i < 4; i++) r_on[i] = s_relays[i].on ? 1 : 0;
    if (s_relay_mutex) xSemaphoreGive(s_relay_mutex);

    time_t now;
    time(&now);

    flower_status_report_t sr = FLOWER_STATUS_REPORT_INIT_ZERO;
    sr.timestamp       = (int64_t)now * 1000;
    sr.has_version     = true;
    sr.version.major   = 1;
    sr.version.minor   = 0;
    sr.version.patch   = 0;
    strncpy(sr.device_type, "C3_relay_four", sizeof(sr.device_type) - 1);
    sr.metrics.arg = s_metrics;
    sr.metrics.funcs.encode = encode_metrics;

    uint8_t buf[512];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, &flower_status_report_t_msg, &sr)) {
        ESP_LOGE(TAG, "Status encode: %s", PB_GET_ERROR(&stream));
        if (s_status_mutex) xSemaphoreGive(s_status_mutex);
        return;
    }
    esp_mqtt_client_publish(s_mqtt_client, s_topic_status,
                            (const char *)buf, (int)stream.bytes_written, 1, 0);

    ESP_LOGI(TAG, "Status published (relays: %d,%d,%d,%d)",
             r_on[0], r_on[1], r_on[2], r_on[3]);

    if (s_status_mutex) xSemaphoreGive(s_status_mutex);
}

/* ── MQTT ────────────────────────────────────────────────────────────────── */
static esp_timer_handle_t s_reconnect_timer = NULL;
static esp_timer_handle_t s_status_timer    = NULL;

static void mqtt_start(void);

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
        esp_timer_stop(s_reconnect_timer);
        esp_mqtt_client_subscribe(s_mqtt_client, s_topic_cmd, 1);
        ESP_LOGI(TAG, "MQTT connected, sub: %s", s_topic_cmd);
        publish_status_report();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected, reconnect in 5 s");
        esp_timer_stop(s_reconnect_timer);
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
    time_t now;
    time(&now);
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
static const char STATUS_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"zh\">\n"
    "<head>\n"
    "<meta charset=\"UTF-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>4路继电器控制面板</title>\n"
    "<style>\n"
    "body{font-family:sans-serif;margin:0;padding:16px;background:#111;color:#eee}\n"
    "h2{margin:0 0 16px;font-size:20px;text-align:center}\n"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:16px;max-width:960px;margin:0 auto}\n"
    ".card{padding:16px;background:#1e1e1e;border-radius:8px;border:1px solid #333}\n"
    ".card h3{margin:0 0 10px;font-size:16px;display:flex;justify-content:space-between;align-items:center}\n"
    ".badge{padding:3px 8px;border-radius:4px;font-size:12px;font-weight:bold}\n"
    ".badge.on{background:#2e7d32;color:#fff}\n"
    ".badge.off{background:#555;color:#bbb}\n"
    ".info{font-size:13px;color:#aaa;margin-bottom:12px;line-height:1.5}\n"
    ".row{display:flex;align-items:center;gap:8px;margin-bottom:10px}\n"
    ".row input{width:70px;padding:6px;background:#2a2a2a;color:#eee;border:1px solid #555;border-radius:4px;text-align:center;font-size:14px}\n"
    ".row label{font-size:13px;color:#888}\n"
    ".btns{display:flex;gap:10px}\n"
    ".btn{flex:1;padding:8px 0;border:none;border-radius:4px;font-size:14px;cursor:pointer;font-weight:bold}\n"
    ".btn-on{background:#2e7d32;color:#fff}\n"
    ".btn-on:active{background:#1b5e20}\n"
    ".btn-off{background:#c62828;color:#fff}\n"
    ".btn-off:active{background:#8e0000}\n"
    "#status{margin-top:16px;font-size:12px;color:#777;text-align:center}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<h2>4路继电器控制面板</h2>\n"
    "<div class=\"grid\">\n"
    "  <div class=\"card\" id=\"relay-card-1\">\n"
    "    <h3><span>继电器 1 [GPIO 3]</span><span class=\"badge off\" id=\"badge-1\">关</span></h3>\n"
    "    <div class=\"info\" id=\"info-1\">状态：加载中...</div>\n"
    "    <div class=\"row\">\n"
    "      <label>开启时长：</label>\n"
    "      <input type=\"number\" id=\"dur-1\" value=\"10\" min=\"1\" max=\"3600\">\n"
    "      <label>秒</label>\n"
    "    </div>\n"
    "    <div class=\"btns\">\n"
    "      <button class=\"btn btn-on\" onclick=\"ctrl(1,'on')\">开</button>\n"
    "      <button class=\"btn btn-off\" onclick=\"ctrl(1,'off')\">关</button>\n"
    "    </div>\n"
    "  </div>\n"
    "  <div class=\"card\" id=\"relay-card-2\">\n"
    "    <h3><span>继电器 2 [GPIO 4]</span><span class=\"badge off\" id=\"badge-2\">关</span></h3>\n"
    "    <div class=\"info\" id=\"info-2\">状态：加载中...</div>\n"
    "    <div class=\"row\">\n"
    "      <label>开启时长：</label>\n"
    "      <input type=\"number\" id=\"dur-2\" value=\"10\" min=\"1\" max=\"3600\">\n"
    "      <label>秒</label>\n"
    "    </div>\n"
    "    <div class=\"btns\">\n"
    "      <button class=\"btn btn-on\" onclick=\"ctrl(2,'on')\">开</button>\n"
    "      <button class=\"btn btn-off\" onclick=\"ctrl(2,'off')\">关</button>\n"
    "    </div>\n"
    "  </div>\n"
    "  <div class=\"card\" id=\"relay-card-3\">\n"
    "    <h3><span>继电器 3 [GPIO 5]</span><span class=\"badge off\" id=\"badge-3\">关</span></h3>\n"
    "    <div class=\"info\" id=\"info-3\">状态：加载中...</div>\n"
    "    <div class=\"row\">\n"
    "      <label>开启时长：</label>\n"
    "      <input type=\"number\" id=\"dur-3\" value=\"10\" min=\"1\" max=\"3600\">\n"
    "      <label>秒</label>\n"
    "    </div>\n"
    "    <div class=\"btns\">\n"
    "      <button class=\"btn btn-on\" onclick=\"ctrl(3,'on')\">开</button>\n"
    "      <button class=\"btn btn-off\" onclick=\"ctrl(3,'off')\">关</button>\n"
    "    </div>\n"
    "  </div>\n"
    "  <div class=\"card\" id=\"relay-card-4\">\n"
    "    <h3><span>继电器 4 [GPIO 7]</span><span class=\"badge off\" id=\"badge-4\">关</span></h3>\n"
    "    <div class=\"info\" id=\"info-4\">状态：加载中...</div>\n"
    "    <div class=\"row\">\n"
    "      <label>开启时长：</label>\n"
    "      <input type=\"number\" id=\"dur-4\" value=\"10\" min=\"1\" max=\"3600\">\n"
    "      <label>秒</label>\n"
    "    </div>\n"
    "    <div class=\"btns\">\n"
    "      <button class=\"btn btn-on\" onclick=\"ctrl(4,'on')\">开</button>\n"
    "      <button class=\"btn btn-off\" onclick=\"ctrl(4,'off')\">关</button>\n"
    "    </div>\n"
    "  </div>\n"
    "</div>\n"
    "<div id=\"status\">正在连接...</div>\n"
    "<script>\n"
    "const urls=['/change_relay1','/change_relay2','/change_relay3','/change_relay4'];\n"
    "async function ctrl(ch,act){\n"
    "  const inp=document.getElementById('dur-'+ch);\n"
    "  const sec=Math.max(1,Math.min(3600,parseInt(inp?inp.value:'10',10)||10));\n"
    "  const url=urls[ch-1]+(act==='off'?'?action=off':('?duration='+(sec*1000)));\n"
    "  try{\n"
    "    const r=await fetch(url);\n"
    "    if(!r.ok)throw new Error('HTTP '+r.status);\n"
    "    poll();\n"
    "  }catch(e){\n"
    "    document.getElementById('status').textContent='控制失败: '+e.message;\n"
    "  }\n"
    "}\n"
    "async function poll(){\n"
    "  try{\n"
    "    const r=await fetch('/api/sensors');\n"
    "    if(!r.ok)throw new Error('HTTP '+r.status);\n"
    "    const d=await r.json();\n"
    "    if(d.relays&&Array.isArray(d.relays)){\n"
    "      for(let i=0;i<d.relays.length;i++){\n"
    "        const item=d.relays[i];\n"
    "        const ch=item.ch||(i+1);\n"
    "        const bd=document.getElementById('badge-'+ch);\n"
    "        const inf=document.getElementById('info-'+ch);\n"
    "        if(bd){\n"
    "          bd.textContent=item.state?'开':'关';\n"
    "          bd.className='badge '+(item.state?'on':'off');\n"
    "        }\n"
    "        if(inf){\n"
    "          if(item.last_on_ts>0){\n"
    "            const dt=new Date(item.last_on_ts*1000).toLocaleString();\n"
    "            const dur=(item.last_on_dur_ms/1000).toFixed(1);\n"
    "            inf.textContent='上次触发: '+dt+' ('+dur+'秒)';\n"
    "          }else{\n"
    "            inf.textContent='上次触发: 从未';\n"
    "          }\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    document.getElementById('status').textContent='最后更新: '+new Date().toLocaleTimeString();\n"
    "  }catch(e){\n"
    "    document.getElementById('status').textContent='获取状态失败: '+e.message;\n"
    "  }\n"
    "}\n"
    "poll();\n"
    "setInterval(poll,2000);\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, STATUS_HTML, sizeof(STATUS_HTML) - 1);
    return ESP_OK;
}

static esp_err_t handle_change_relay_ch(httpd_req_t *req, int ch)
{
    if (ch < 0 || ch >= 4) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char query[64] = {};
    uint32_t dur = RELAY_DEFAULT_DURATION_MS;
    if (s_relay_mutex) xSemaphoreTake(s_relay_mutex, portMAX_DELAY);
    if (s_relays[ch].duration_ms > 0) {
        dur = s_relays[ch].duration_ms;
    }
    if (s_relay_mutex) xSemaphoreGive(s_relay_mutex);
    bool is_off = false;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16] = {};
        if (httpd_query_key_value(query, "action", val, sizeof(val)) == ESP_OK) {
            if (strcmp(val, "off") == 0) {
                is_off = true;
            }
        }
        if (!is_off && httpd_query_key_value(query, "duration", val, sizeof(val)) == ESP_OK) {
            uint32_t d = (uint32_t)atoi(val);
            if (d > 0) dur = d;
        }
    }

    if (is_off) {
        relay_set(ch, false, 0);
    } else {
        relay_set(ch, true, dur);
    }

    httpd_resp_send(req, "0", 1);
    return ESP_OK;
}

static esp_err_t handle_change_relay1(httpd_req_t *req) { return handle_change_relay_ch(req, 0); }
static esp_err_t handle_change_relay2(httpd_req_t *req) { return handle_change_relay_ch(req, 1); }
static esp_err_t handle_change_relay3(httpd_req_t *req) { return handle_change_relay_ch(req, 2); }
static esp_err_t handle_change_relay4(httpd_req_t *req) { return handle_change_relay_ch(req, 3); }

static esp_err_t handle_api_sensors(httpd_req_t *req)
{
    char json[512];
    if (s_relay_mutex) xSemaphoreTake(s_relay_mutex, portMAX_DELAY);
    snprintf(json, sizeof(json),
        "{"
        "\"relays\":["
        "{\"ch\":1,\"gpio\":3,\"state\":%d,\"last_on_ts\":%lld,\"last_on_dur_ms\":%lu},"
        "{\"ch\":2,\"gpio\":4,\"state\":%d,\"last_on_ts\":%lld,\"last_on_dur_ms\":%lu},"
        "{\"ch\":3,\"gpio\":5,\"state\":%d,\"last_on_ts\":%lld,\"last_on_dur_ms\":%lu},"
        "{\"ch\":4,\"gpio\":7,\"state\":%d,\"last_on_ts\":%lld,\"last_on_dur_ms\":%lu}"
        "],"
        "\"relay_last_on_ts\":%lld,"
        "\"relay_last_on_dur_ms\":%lu"
        "}",
        s_relays[0].on ? 1 : 0, (long long)s_relays[0].last_on_utc_s, (unsigned long)s_relays[0].last_on_dur_ms,
        s_relays[1].on ? 1 : 0, (long long)s_relays[1].last_on_utc_s, (unsigned long)s_relays[1].last_on_dur_ms,
        s_relays[2].on ? 1 : 0, (long long)s_relays[2].last_on_utc_s, (unsigned long)s_relays[2].last_on_dur_ms,
        s_relays[3].on ? 1 : 0, (long long)s_relays[3].last_on_utc_s, (unsigned long)s_relays[3].last_on_dur_ms,
        (long long)s_relays[0].last_on_utc_s,
        (unsigned long)s_relays[0].last_on_dur_ms);
    if (s_relay_mutex) xSemaphoreGive(s_relay_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static void http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return;
    }
    static const httpd_uri_t uri_root = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = handle_root,
    };
    static const httpd_uri_t uri_relay1 = {
        .uri     = "/change_relay1",
        .method  = HTTP_GET,
        .handler = handle_change_relay1,
    };
    static const httpd_uri_t uri_relay2 = {
        .uri     = "/change_relay2",
        .method  = HTTP_GET,
        .handler = handle_change_relay2,
    };
    static const httpd_uri_t uri_relay3 = {
        .uri     = "/change_relay3",
        .method  = HTTP_GET,
        .handler = handle_change_relay3,
    };
    static const httpd_uri_t uri_relay4 = {
        .uri     = "/change_relay4",
        .method  = HTTP_GET,
        .handler = handle_change_relay4,
    };
    static const httpd_uri_t uri_api_sensors = {
        .uri     = "/api/sensors",
        .method  = HTTP_GET,
        .handler = handle_api_sensors,
    };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_relay1);
    httpd_register_uri_handler(server, &uri_relay2);
    httpd_register_uri_handler(server, &uri_relay3);
    httpd_register_uri_handler(server, &uri_relay4);
    httpd_register_uri_handler(server, &uri_api_sensors);
    ESP_LOGI(TAG, "HTTP server started on port 80");
}

/* ── App timer: LED breath, button poll, relay auto-off ──────────────────── */
static esp_timer_handle_t s_app_timer = NULL;
static int64_t            s_app_tick  = 0;

#define BREATH_STEPS (BREATH_PERIOD_MS / APP_TIMER_INTERVAL_MS)  /* 60 steps */

static void app_timer_cb(void *arg)
{
    s_app_tick++;

    /* Breathing LED on GPIO 2 */
    float phase = (float)(s_app_tick % BREATH_STEPS) / (float)BREATH_STEPS
                  * 2.0f * (float)M_PI;
    uint32_t duty = (uint32_t)(((sinf(phase - (float)M_PI / 2.0f) + 1.0f) / 2.0f) * 255.0f);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

    /* For ch = 0 to 3 */
    if (s_relay_mutex) xSemaphoreTake(s_relay_mutex, portMAX_DELAY);
    for (int ch = 0; ch < 4; ch++) {
        /* Scan button with debounce: active-low */
        bool raw_btn = (bool)gpio_get_level(s_relays[ch].button_gpio);
        if (raw_btn != s_relays[ch].last_button_state) {
            s_relays[ch].btn_debounce_cnt++;
            if (s_relays[ch].btn_debounce_cnt >= 2) {
                s_relays[ch].last_button_state = raw_btn;
                s_relays[ch].btn_debounce_cnt = 0;
                if (!raw_btn) {
                    relay_toggle_unlocked(ch);
                }
            }
        } else {
            s_relays[ch].btn_debounce_cnt = 0;
        }

        /* Auto-off check */
        if (s_relays[ch].on && s_relays[ch].duration_ms > 0) {
            int64_t elapsed_ms = (esp_timer_get_time() - s_relays[ch].on_us) / 1000LL;
            if (elapsed_ms >= (int64_t)s_relays[ch].duration_ms) {
                relay_set_unlocked(ch, false, 0);
            }
        }
    }
    if (s_relay_mutex) xSemaphoreGive(s_relay_mutex);
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
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, s_host_name);

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

    /* Mutexes */
    s_relay_mutex = xSemaphoreCreateMutex();
    s_status_mutex = xSemaphoreCreateMutex();
    configASSERT(s_relay_mutex != NULL && s_status_mutex != NULL);

    /* Factory partition — device identity (id + secret) */
    nvs_flash_init_partition("fctry");
    if (device_identity_init() != ESP_OK) {
        ESP_LOGE(TAG, "Fatal: invalid or missing device identity, halting");
        return;
    }

    /* GPIO: 4 Relays (GPIO 3, 4, 5, 7: output, default off) */
    uint64_t relay_mask = (1ULL << GPIO_NUM_3) |
                          (1ULL << GPIO_NUM_4) |
                          (1ULL << GPIO_NUM_5) |
                          (1ULL << GPIO_NUM_7);
    gpio_config_t relay_conf = {
        .pin_bit_mask = relay_mask,
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < 4; i++) {
        gpio_set_level(s_relays[i].relay_gpio, 0);
    }
    ESP_ERROR_CHECK(gpio_config(&relay_conf));

    /* GPIO: 4 Buttons (GPIO 10, 9, 6, 8: input pull-up) */
    uint64_t btn_mask = (1ULL << GPIO_NUM_10) |
                        (1ULL << GPIO_NUM_9)  |
                        (1ULL << GPIO_NUM_6)  |
                        (1ULL << GPIO_NUM_8);
    gpio_config_t btn_conf = {
        .pin_bit_mask = btn_mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_conf));

    /* LEDC: breathing LED on GPIO 2 */
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

    /* App timer: 50 ms — LED breath, buttons, relay auto-off */
    esp_timer_create_args_t app_args = {
        .callback              = app_timer_cb,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "app",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&app_args, &s_app_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_app_timer,
                        (uint64_t)APP_TIMER_INTERVAL_MS * 1000ULL));

    /* Status timer: 30 s — status report */
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

    /* OTA task */
    s_ota_url_queue = xQueueCreate(1, OTA_URL_MAX);
    xTaskCreate(ota_task, "ota", OTA_TASK_STACK, NULL, 5, NULL);

    /* WiFi */
    wifi_init_sta();

    /* SNTP (UTC+8) */
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
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
