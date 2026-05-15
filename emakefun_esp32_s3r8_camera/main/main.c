/**
 * main.c — Flower ESP32-S3 camera node (emakefun ESP32-S3-R8, OV3660)
 *
 * Features:
 *   1. WiFi STA + NTP time sync
 *   2. HMAC-SHA256 MQTT credentials (clientId{id}timestamp{ms})
 *   3. MQTT 5.0 with User Property {"project","flower"} on CONNECT
 *   4. nanopb: receives snapshot/ota commands; sends status_report + cmd_response
 *   5. OV3660 DVP camera via esp_video / V4L2
 *   6. Snapshot: JPEG capture + HTTP PUT to pre-signed URL (flash LED during capture)
 *   7. OTA: HTTPS firmware update triggered via MQTT OtaCommand
 *
 * Topics:
 *   sub  flower/{DEVICE_ID}/down/cmd        — Command protobuf (snapshot / ota)
 *   pub  flower/{DEVICE_ID}/up/status       — periodic StatusReport
 *   pub  flower/{DEVICE_ID}/up/cmd_response — CommandResponse (snapshot / ota)
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include "camera.h"
#include "ds18b20.h"
#include "build_info.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "nvs_flash.h"

#include "mqtt_client.h"
#include "mqtt5_client.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"

#include "psa/crypto.h"

#include "pb_encode.h"
#include "pb_decode.h"
#include "flower.pb.h"

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

/* ── Device identity ─────────────────────────────────────────────────────── */
static char g_device_id[64];
static char g_device_secret[128];

static char s_topic_cmd[96];
static char s_topic_status[96];
static char s_topic_cmd_resp[96];

/* ── Hardware ────────────────────────────────────────────────────────────── */
#define STATUS_INTERVAL_MS  30000
#define STATUS_BUF_SIZE     256

#ifndef CONFIG_DS18B20_GPIO
#define CONFIG_DS18B20_GPIO  46
#endif

#define TAG "CamMqtt"

/* ── State ───────────────────────────────────────────────────────────────── */
static EventGroupHandle_t       s_wifi_event_group;
#define WIFI_CONNECTED_BIT      BIT0

static esp_mqtt_client_handle_t s_mqtt_client    = NULL;
static volatile bool            s_mqtt_connected = false;

static flower_command_t         s_cmd;

typedef struct {
    flower_snapshot_command_t cmd;
    flower_command_role_t     role;
} snap_req_t;

static QueueHandle_t     s_snap_queue;
static volatile bool     s_snap_in_progress = false;

/* ── HMAC-SHA256 ─────────────────────────────────────────────────────────── */
static void calc_signature(int64_t ts_ms, const char *device_id,
                            const char *secret, char out[65])
{
    char plaintext[128];
    snprintf(plaintext, sizeof(plaintext),
             "clientId%stimestamp%lld", device_id, (long long)ts_ms);

    uint8_t hmac[32];
    size_t  hmac_len;
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

/* ── Device identity ─────────────────────────────────────────────────────── */
static void device_identity_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition("fctry", "identity", NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fctry open failed (%s), identity unavailable",
                 esp_err_to_name(err));
        goto build_topics;
    }
    size_t len = sizeof(g_device_id);
    nvs_get_str(h, "device_id",     g_device_id,     &len);
    len = sizeof(g_device_secret);
    nvs_get_str(h, "device_secret", g_device_secret, &len);
    nvs_close(h);
    ESP_LOGI(TAG, "Identity loaded: id=%s", g_device_id);

build_topics:
    snprintf(s_topic_cmd,      sizeof(s_topic_cmd),      "flower/%s/down/cmd",        g_device_id);
    snprintf(s_topic_status,   sizeof(s_topic_status),   "flower/%s/up/status",       g_device_id);
    snprintf(s_topic_cmd_resp, sizeof(s_topic_cmd_resp), "flower/%s/up/cmd_response", g_device_id);
}

/* ── Status report ───────────────────────────────────────────────────────── */
static flower_metric_t s_metrics[1];

static bool encode_metrics(pb_ostream_t *stream, const pb_field_t *field,
                           void *const *arg)
{
    flower_metric_t *metrics = (flower_metric_t *)(*arg);
    for (int i = 0; i < 1; i++) {
        if (!pb_encode_tag_for_field(stream, field))
            return false;
        if (!pb_encode_submessage(stream, FLOWER_METRIC_FIELDS, &metrics[i]))
            return false;
    }
    return true;
}

static void publish_status_report(void)
{
    float temp = ds18b20_read_temperature();

    if (temp > -273.0f) {
        strcpy(s_metrics[0].key, "temp");
        s_metrics[0].which_value = FLOWER_METRIC_DOUBLE_VALUE_TAG;
        s_metrics[0].value.double_value = (double)temp;
    } else {
        strcpy(s_metrics[0].key, "temp");
        s_metrics[0].which_value = FLOWER_METRIC_INT64_VALUE_TAG;
        s_metrics[0].value.int64_value = 0;
    }

    if (!s_mqtt_connected) return;

    time_t now; time(&now);

    flower_status_report_t sr = FLOWER_STATUS_REPORT_INIT_ZERO;
    sr.timestamp       = (int64_t)now * 1000;
    sr.has_version     = true;
    sr.version.major   = 1;
    sr.version.minor   = 0;
    sr.version.patch   = 0;
    strcpy(sr.device_type, "camera");
    sr.metrics.arg = s_metrics;
    sr.metrics.funcs.encode = encode_metrics;

    uint8_t buf[STATUS_BUF_SIZE];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, &flower_status_report_t_msg, &sr)) {
        ESP_LOGE(TAG, "Status encode: %s", PB_GET_ERROR(&stream));
        return;
    }
    esp_mqtt_client_publish(s_mqtt_client, s_topic_status,
                            (const char *)buf, (int)stream.bytes_written, 1, 0);

    if (temp > -273.0f)
        ESP_LOGI(TAG, "Status published (temp=%.1f C)", temp);
    else
        ESP_LOGI(TAG, "Status published (temp=n/a)");
}

/* ── Snapshot ────────────────────────────────────────────────────────────── */
static void publish_snapshot_response(flower_command_role_t role,
                                      flower_snapshot_result_code_t result,
                                      uint32_t size, const char *reason)
{
    flower_command_response_t resp = FLOWER_COMMAND_RESPONSE_INIT_ZERO;
    resp.role                    = role;
    resp.which_payload           = FLOWER_COMMAND_RESPONSE_SNAPSHOT_TAG;
    resp.payload.snapshot.result = result;
    resp.payload.snapshot.size   = size;
    if (reason)
        snprintf(resp.payload.snapshot.reason,
                 sizeof(resp.payload.snapshot.reason), "%s", reason);

    uint8_t buf[FLOWER_COMMAND_RESPONSE_SIZE];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, &flower_command_response_t_msg, &resp)) {
        ESP_LOGE(TAG, "Snap resp encode: %s", PB_GET_ERROR(&stream));
        return;
    }
    esp_mqtt_client_publish(s_mqtt_client, s_topic_cmd_resp,
                            (const char *)buf, (int)stream.bytes_written, 1, 0);
}

static void snapshot_task(void *arg)
{
    snap_req_t req;
    for (;;) {
        xQueueReceive(s_snap_queue, &req, portMAX_DELAY);
        s_snap_in_progress = true;

        const uint8_t *jpeg  = NULL;
        uint32_t       jsize = 0;
        esp_err_t cerr = camera_capture_jpeg(&jpeg, &jsize);
        if (cerr != ESP_OK) {
            publish_snapshot_response(req.role,
                FLOWER_SNAPSHOT_RESULT_CODE_SNAPSHOT_CAPTURE_FAILED, 0, "capture failed");
            s_snap_in_progress = false;
            continue;
        }
        ESP_LOGI(TAG, "Captured %u B JPEG, uploading to: %.80s...", jsize, req.cmd.upload_url);

        esp_http_client_config_t http_cfg = {
            .url                         = req.cmd.upload_url,
            .method                      = HTTP_METHOD_PUT,
            .crt_bundle_attach           = esp_crt_bundle_attach,
            .skip_cert_common_name_check = true,
            .timeout_ms                  = 30000,
            .buffer_size_tx              = 4096,
        };
        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        esp_http_client_set_header(client, "Content-Type", "image/jpeg");
        esp_http_client_set_post_field(client, (const char *)jpeg, (int)jsize);
        esp_err_t herr = esp_http_client_perform(client);
        int       hstatus = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        camera_release_jpeg();

        if (herr != ESP_OK) {
            ESP_LOGE(TAG, "HTTP PUT error: %s", esp_err_to_name(herr));
            publish_snapshot_response(req.role,
                FLOWER_SNAPSHOT_RESULT_CODE_SNAPSHOT_UPLOAD_FAILED, 0,
                esp_err_to_name(herr));
        } else if (hstatus / 100 != 2) {
            ESP_LOGE(TAG, "HTTP PUT rejected: %d", hstatus);
            char reason[24];
            snprintf(reason, sizeof(reason), "HTTP %d", hstatus);
            publish_snapshot_response(req.role,
                FLOWER_SNAPSHOT_RESULT_CODE_SNAPSHOT_UPLOAD_REJECTED, 0, reason);
        } else {
            ESP_LOGI(TAG, "Snapshot uploaded: %u B, HTTP %d", jsize, hstatus);
            publish_snapshot_response(req.role,
                FLOWER_SNAPSHOT_RESULT_CODE_SNAPSHOT_OK, jsize, NULL);
        }

        s_snap_in_progress = false;
    }
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
    resp.which_payload        = FLOWER_COMMAND_RESPONSE_OTA_TAG;
    resp.payload.ota.status   = status;
    resp.payload.ota.progress = progress;
    if (msg)
        strncpy(resp.payload.ota.message, msg, sizeof(resp.payload.ota.message) - 1);

    uint8_t buf[FLOWER_COMMAND_RESPONSE_SIZE];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, &flower_command_response_t_msg, &resp)) {
        ESP_LOGE(TAG, "OTA resp encode: %s", PB_GET_ERROR(&stream));
        return;
    }
    esp_mqtt_client_publish(s_mqtt_client, s_topic_cmd_resp,
                            (const char *)buf, (int)stream.bytes_written, 1, 0);
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
            .url                         = url,
            .timeout_ms                  = 30000,
            .keep_alive_enable           = true,
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
                int read   = esp_https_ota_get_image_len_read(handle);
                int pct    = (int)((int64_t)read * 100 / img_size);
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

/* ── HTTP server ──────────────────────────────────────────────────────────── */
static httpd_handle_t s_httpd = NULL;

static esp_err_t http_snapshot_handler(httpd_req_t *req)
{
    const uint8_t *jpeg = NULL;
    uint32_t jsize = 0;
    esp_err_t ret = camera_capture_jpeg(&jpeg, &jsize);
    if (ret != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_send(req, (const char *)jpeg, (int)jsize);
    camera_release_jpeg();
    return ESP_OK;
}

static esp_err_t http_root_handler(httpd_req_t *req)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"device\":\"%s\",\"camera\":\"%s\",\"mqtt\":\"%s\"}",
        g_device_id,
        camera_is_ready() ? "OK" : "NONE",
        s_mqtt_connected ? "UP" : "DOWN");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static void http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return;
    }
    httpd_uri_t root_uri = { .uri = "/",         .method = HTTP_GET, .handler = http_root_handler };
    httpd_uri_t snap_uri = { .uri = "/snapshot", .method = HTTP_GET, .handler = http_snapshot_handler };
    httpd_register_uri_handler(s_httpd, &root_uri);
    httpd_register_uri_handler(s_httpd, &snap_uri);
    ESP_LOGI(TAG, "HTTP server started on port %d", cfg.server_port);
}

/* ── MQTT message handler ────────────────────────────────────────────────── */
static void handle_mqtt_data(const char *topic, int topic_len,
                              const char *data,  int data_len)
{
    if (topic_len != (int)strlen(s_topic_cmd) ||
        memcmp(topic, s_topic_cmd, topic_len) != 0) {
        return;
    }

    memset(&s_cmd, 0, sizeof(s_cmd));
    pb_istream_t stream = pb_istream_from_buffer(
        (const pb_byte_t *)data, data_len);
    if (!pb_decode(&stream, &flower_command_t_msg, &s_cmd)) {
        ESP_LOGE(TAG, "Command decode: %s", PB_GET_ERROR(&stream));
        return;
    }
    switch (s_cmd.which_payload) {
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
    case FLOWER_COMMAND_SNAPSHOT_TAG: {
        if (s_snap_in_progress) {
            ESP_LOGW(TAG, "Snapshot already in progress");
            publish_snapshot_response(s_cmd.role,
                FLOWER_SNAPSHOT_RESULT_CODE_SNAPSHOT_BUSY, 0, "busy");
            break;
        }
        if (s_cmd.payload.snapshot.upload_url[0] == '\0') {
            ESP_LOGE(TAG, "Snapshot: empty upload_url");
            publish_snapshot_response(s_cmd.role,
                FLOWER_SNAPSHOT_RESULT_CODE_SNAPSHOT_INVALID_URL, 0, "empty url");
            break;
        }
        snap_req_t req;
        req.cmd  = s_cmd.payload.snapshot;
        req.role = s_cmd.role;
        if (xQueueSend(s_snap_queue, &req, 0) != pdTRUE)
            ESP_LOGE(TAG, "Snap queue full");
        break;
    }
    default:
        ESP_LOGW(TAG, "Unhandled cmd tag=%d", (int)s_cmd.which_payload);
        break;
    }
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
        esp_mqtt_client_subscribe(s_mqtt_client, s_topic_cmd, 1);
        ESP_LOGI(TAG, "MQTT connected, sub: %s", s_topic_cmd);
        publish_status_report();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected, reconnect in 5 s");
        esp_timer_start_once(s_reconnect_timer, 5000000ULL);
        break;
    case MQTT_EVENT_DATA:
        handle_mqtt_data(event->topic, event->topic_len,
                         event->data,  event->data_len);
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
    mqtt_cfg.broker.address.uri                              = CONFIG_MQTT_BROKER_URI;
    mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
    mqtt_cfg.credentials.client_id                           = g_device_id;
    mqtt_cfg.credentials.username                            = s_mqtt_username;
    mqtt_cfg.credentials.authentication.password             = s_mqtt_password;
    mqtt_cfg.session.keepalive                               = 60;
    mqtt_cfg.session.protocol_ver                            = MQTT_PROTOCOL_V_5;
    mqtt_cfg.network.disable_auto_reconnect                  = true;
    mqtt_cfg.buffer.out_size = 65536;

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);

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
    ESP_LOGI(TAG, "Firmware built: %s", BUILD_TIMESTAMP);

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* PSA Crypto */
    ESP_ERROR_CHECK(psa_crypto_init());

    /* Factory partition — device identity */
    nvs_flash_init_partition("fctry");
    device_identity_init();

    /* Camera init (creates own I2C bus using Kconfig pins) */
    ret = camera_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed (%s) — continuing without camera",
                 esp_err_to_name(ret));
    }

    /* DS18B20 temperature sensor */
    if (CONFIG_DS18B20_GPIO >= 0) {
        ds18b20_init((gpio_num_t)CONFIG_DS18B20_GPIO);
    }

    /* Snapshot queue + task */
    s_snap_queue = xQueueCreate(1, sizeof(snap_req_t));
    xTaskCreate(snapshot_task, "snap", 8192, NULL, 5, NULL);

    /* OTA queue + task */
    s_ota_url_queue = xQueueCreate(1, OTA_URL_MAX);
    xTaskCreate(ota_task, "ota", OTA_TASK_STACK, NULL, 5, NULL);

    /* Status timer: 30 s */
    esp_timer_create_args_t st_args = {
        .callback              = status_timer_cb,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "status",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&st_args, &s_status_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_status_timer,
                        (uint64_t)STATUS_INTERVAL_MS * 1000ULL));

    /* Reconnect timer */
    esp_timer_create_args_t rc_args = {
        .callback        = reconnect_timer_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&rc_args, &s_reconnect_timer));

    /* WiFi */
    wifi_init_sta();

    /* HTTP server */
    http_server_start();

    /* NTP */
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

    /* Idle */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "Uptime: %lld s, cam=%s, mqtt=%s",
                 esp_timer_get_time() / 1000000LL,
                 camera_is_ready() ? "OK" : "NONE",
                 s_mqtt_connected ? "UP" : "DOWN");
    }
}
