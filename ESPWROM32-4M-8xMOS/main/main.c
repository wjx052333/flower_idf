/**
 * main.c — Flower ESP32 relay controller (ESP-IDF)
 *
 * Features:
 *   1. WiFi STA + NTP time sync
 *   2. HMAC-SHA256 MQTT credentials (clientId{id}timestamp{ms})
 *   3. MQTT 5.0 with User Property {"project","flower"} on CONNECT
 *   4. nanopb: receives relay_control/ota commands; sends status_report + cmd_response
 *   5. GPIO: relay (pin 4, active-low), button (pin 9), breathing LED (pin 3), DS18B20 temp (pin 1)
 *   6. HTTP server: GET /change_relay1[?duration=ms]
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
#include <math.h>
#include <time.h>

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

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"

#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"

#include "mqtt_client.h"
#include "mqtt5_client.h"

#include "psa/crypto.h"

#include "pb_encode.h"
#include "pb_decode.h"
#include "flower.pb.h"
#include "ds18b20.h"
#include "aht25.h"
#include "water_logic.h"
#include "water_store.h"

#include "device_discovery.h"

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
static char s_host_name[96];

/* ── Hardware (ESP32-WROOM-32E + 8x MOS) ─────────────────────────────────── */
#define NUM_RELAYS            8
#define MOS1_PIN              19
#define MOS2_PIN              18
#define MOS3_PIN              17
#define MOS4_PIN              16
#define MOS5_PIN              33
#define MOS6_PIN              25
#define MOS7_PIN              26
#define MOS8_PIN              27

/* Relay 1 is MOS1 (also default for auto watering / board button) */
#define RELAY_PIN             MOS1_PIN

#define BUTTON_PIN            0   /* Board Key (Boot button) */
#define LED_PIN               23  /* Board User LED */
#define REED_PIN              GPIO_NUM_14 /* Reed switch input (internal pull-up) */
#define ADC_PIN_LOW_HUMIDITY  ADC_CHANNEL_6   /* GPIO34 (ADC1_CH6) */
#define DS18B20_PIN           GPIO_NUM_4      /* GPIO4 */

#define STATUS_INTERVAL_MS        30000
#define SENSOR_MUTEX_TIMEOUT_MS   3000
#define RELAY_DEFAULT_DURATION_MS 10000
#define BREATH_PERIOD_MS          3000
#define APP_TIMER_INTERVAL_MS     50   /* LED + button + relay auto-off tick */

#define TAG "Flower"

/* ── Firmware version ─────────────────────────────────────────────────────── */
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 2
#define FW_VERSION_PATCH 0

#define _STR(x) #x
#define STR(x) _STR(x)
#define FW_VERSION_STR STR(FW_VERSION_MAJOR) "." STR(FW_VERSION_MINOR) "." STR(FW_VERSION_PATCH)

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

    snprintf(s_host_name,      sizeof(s_host_name),      "mos_%s",                    g_device_id);
}

/* ── State ───────────────────────────────────────────────────────────────── */
static EventGroupHandle_t       s_wifi_event_group;
#define WIFI_CONNECTED_BIT      BIT0

static SemaphoreHandle_t g_sensor_mutex = NULL;

static esp_mqtt_client_handle_t s_mqtt_client    = NULL;
static volatile bool            s_mqtt_connected = false;

typedef struct {
    gpio_num_t       relay_gpio;
    bool             on;
    int64_t          on_us;
    uint32_t         duration_ms;
    int64_t          last_on_utc_s;
    uint32_t         last_on_dur_ms;
} relay_channel_t;

static SemaphoreHandle_t s_relay_mutex = NULL;

static relay_channel_t s_relays[NUM_RELAYS] = {
    { .relay_gpio = MOS1_PIN, .on = false, .duration_ms = RELAY_DEFAULT_DURATION_MS },
    { .relay_gpio = MOS2_PIN, .on = false, .duration_ms = RELAY_DEFAULT_DURATION_MS },
    { .relay_gpio = MOS3_PIN, .on = false, .duration_ms = RELAY_DEFAULT_DURATION_MS },
    { .relay_gpio = MOS4_PIN, .on = false, .duration_ms = RELAY_DEFAULT_DURATION_MS },
    { .relay_gpio = MOS5_PIN, .on = false, .duration_ms = RELAY_DEFAULT_DURATION_MS },
    { .relay_gpio = MOS6_PIN, .on = false, .duration_ms = RELAY_DEFAULT_DURATION_MS },
    { .relay_gpio = MOS7_PIN, .on = false, .duration_ms = RELAY_DEFAULT_DURATION_MS },
    { .relay_gpio = MOS8_PIN, .on = false, .duration_ms = RELAY_DEFAULT_DURATION_MS },
};

static void relay_set_unlocked(int ch, bool on, uint32_t duration_ms)
{
    if (ch < 0 || ch >= NUM_RELAYS) return;

    /* Interlock: if reed switch is open (level != 0), forbid turning on MOS 2 and trigger MOS 3 for 1s */
    if (ch == 1 && on) {
        if (gpio_get_level(REED_PIN) != 0) {
            ESP_LOGW(TAG, "Reed switch open: rejected turning on MOS 2, triggering MOS 3 for 1s");
            relay_set_unlocked(2, true, 1000);
            return;
        }
    }

    if (on) {
        s_relays[ch].on_us = esp_timer_get_time();
        s_relays[ch].duration_ms = duration_ms;
        time_t _now; time(&_now);
        s_relays[ch].last_on_utc_s = (int64_t)_now;
        s_relays[ch].last_on_dur_ms = duration_ms;
    }
    s_relays[ch].on = on;
    gpio_set_level(s_relays[ch].relay_gpio, on ? 1 : 0);
}

void relay_set(int ch, bool on, uint32_t duration_ms)
{
    if (s_relay_mutex) xSemaphoreTake(s_relay_mutex, portMAX_DELAY);
    relay_set_unlocked(ch, on, duration_ms);
    if (s_relay_mutex) xSemaphoreGive(s_relay_mutex);
}

static void relay_toggle_unlocked(int ch)
{
    if (ch < 0 || ch >= NUM_RELAYS) return;
    if (s_relays[ch].on) {
        relay_set_unlocked(ch, false, 0);
    } else {
        relay_set_unlocked(ch, true, s_relays[ch].duration_ms > 0 ? s_relays[ch].duration_ms : RELAY_DEFAULT_DURATION_MS);
    }
}

void relay_toggle(int ch)
{
    if (s_relay_mutex) xSemaphoreTake(s_relay_mutex, portMAX_DELAY);
    relay_toggle_unlocked(ch);
    if (s_relay_mutex) xSemaphoreGive(s_relay_mutex);
}

/* ── Reed switch interrupt & event task ─────────────────────────────────── */
static TaskHandle_t s_reed_task_handle = NULL;

static void IRAM_ATTR reed_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t high_task_wakeup = pdFALSE;
    if (s_reed_task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_reed_task_handle, &high_task_wakeup);
        portYIELD_FROM_ISR(high_task_wakeup);
    }
}

static void reed_task(void *arg)
{
    (void)arg;
    int64_t last_trigger_us = 0;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Debounce delay: 30 ms */
        vTaskDelay(pdMS_TO_TICKS(30));

        /* Verify that REED_PIN is still open (level == 1) */
        if (gpio_get_level(REED_PIN) == 0) {
            continue; /* False trigger / bounce */
        }

        /* Cooldown: ignore triggers within 200 ms */
        int64_t now_us = esp_timer_get_time();
        if ((now_us - last_trigger_us) < 200000LL) {
            continue;
        }
        last_trigger_us = now_us;

        ESP_LOGI(TAG, "Reed switch opened (closed->open transition detected)");

        if (s_relay_mutex) xSemaphoreTake(s_relay_mutex, portMAX_DELAY);
        /* If MOS 2 is currently ON, turn it OFF immediately */
        if (s_relays[1].on) {
            ESP_LOGW(TAG, "Reed opened while MOS 2 was ON: forcing MOS 2 OFF immediately");
            relay_set_unlocked(1, false, 0);
        }
        /* Trigger MOS 3 for 1 second */
        relay_set_unlocked(2, true, 1000);
        if (s_relay_mutex) xSemaphoreGive(s_relay_mutex);
    }
}

static volatile bool            s_last_btn       = true; /* HIGH = not pressed */

/* Static to avoid large stack allocation */
static flower_command_t          s_cmd;
static flower_command_response_t s_resp;

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
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, ADC_PIN_LOW_HUMIDITY, &chan_cfg));
}

static int adc_read_raw(adc_channel_t channel)
{
    int raw = 0;
    adc_oneshot_read(s_adc_handle, channel, &raw);
    return raw;
}

/* ── I2C sensors (BH1750 illuminance + AHT25 temperature/humidity) ───────── */
#define BH1750_SCL_IO   GPIO_NUM_22
#define BH1750_SDA_IO   GPIO_NUM_21
#define BH1750_ADDR     0x23
#define BH1750_CONT_H   0x10   /* Continuous H-Resolution Mode (~120 ms/sample) */

static i2c_master_bus_handle_t s_i2c_bus    = NULL;
static i2c_master_dev_handle_t s_bh1750_dev = NULL;

static void i2c_sensors_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port             = I2C_NUM_0,
        .sda_io_num           = BH1750_SDA_IO,
        .scl_io_num           = BH1750_SCL_IO,
        .clk_source           = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt    = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BH1750_ADDR,
        .scl_speed_hz    = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_bh1750_dev);
    if (err == ESP_OK) {
        uint8_t cmd = BH1750_CONT_H;
        err = i2c_master_transmit(s_bh1750_dev, &cmd, 1, 100);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "BH1750 initialized on I2C (0x23)");
        } else {
            ESP_LOGW(TAG, "BH1750 start continuous mode failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Failed to add BH1750 to I2C: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(200)); /* wait for first measurement */

    /* Initialize AHT25 on shared I2C bus */
    err = aht25_init(s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AHT25 init returned: %s", esp_err_to_name(err));
    }
}

static float bh1750_read_illuminance(void)
{
    if (!s_bh1750_dev) return -1.0f;
    uint8_t buf[2];
    esp_err_t err = i2c_master_receive(s_bh1750_dev, buf, 2, 200);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BH1750 read error: %s", esp_err_to_name(err));
        return -1.0f;
    }
    uint16_t raw = ((uint16_t)buf[0] << 8) | buf[1];
    return (float)raw / 1.2f;
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

/* ── Status metrics ──────────────────────────────────────────────────────── */
#define METRICS_COUNT 8

static flower_metric_t s_metrics[METRICS_COUNT];

static bool encode_metrics(pb_ostream_t *stream, const pb_field_t *field, void *const *arg)
{
    flower_metric_t *metrics = (flower_metric_t *)(*arg);
    for (int i = 0; i < METRICS_COUNT; i++) {
        if (!pb_encode_tag_for_field(stream, field))
            return false;
        if (!pb_encode_submessage(stream, FLOWER_METRIC_FIELDS, &metrics[i]))
            return false;
    }
    return true;
}

static float calculate_humidity_from_raw(int raw_low)
{
    const water_config_t *cfg = water_config();
    float humidity = water_calc_humidity(raw_low, cfg->raw_dry, cfg->raw_wet);
    ESP_LOGI(TAG, "ADC humidity(GPIO34)=%d, calibrated=%.1f%% (dry=%u, wet=%u)",
             raw_low, humidity, (unsigned)cfg->raw_dry, (unsigned)cfg->raw_wet);

    return humidity;
}

float get_humidity(void) {
    int raw_low = adc_read_raw(ADC_PIN_LOW_HUMIDITY);
    return calculate_humidity_from_raw(raw_low);
}

static void publish_status_report(void)
{
    if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(SENSOR_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Status report: sensor busy, skipping this tick");
        return;
    }
    float humidity = get_humidity();

    float temp = ds18b20_read_temperature();
    if (temp > -273.0f)
        ESP_LOGI(TAG, "Temperature(GPIO1)=%.2f°C", temp);

    float lux = bh1750_read_illuminance();
    if (lux >= 0)
        ESP_LOGI(TAG, "Illuminance=%.3f Lux", lux);

    float aht_temp = 0.0f, aht_humi = 0.0f;
    esp_err_t aht_err = aht25_read(&aht_temp, &aht_humi);
    if (aht_err == ESP_OK)
        ESP_LOGI(TAG, "AHT25 temp=%.2f°C, humi=%.1f%%", aht_temp, aht_humi);
    else
        ESP_LOGW(TAG, "AHT25 read failed: %s", esp_err_to_name(aht_err));

    xSemaphoreGive(g_sensor_mutex);

    if (!s_mqtt_connected) return;

    /* Current and previous 8x MOS status bitmap */
    uint32_t mos_current = 0;
    if (s_relay_mutex) xSemaphoreTake(s_relay_mutex, portMAX_DELAY);
    for (int i = 0; i < NUM_RELAYS; i++) {
        if (s_relays[i].on) {
            mos_current |= (1U << i);
        }
    }
    if (s_relay_mutex) xSemaphoreGive(s_relay_mutex);

    static uint32_t s_prev_mos_state = 0;
    uint32_t mos_prev = s_prev_mos_state;

    strcpy(s_metrics[0].key, "humidity");
    s_metrics[0].which_value = FLOWER_METRIC_DOUBLE_VALUE_TAG;
    s_metrics[0].value.double_value = humidity;

    strcpy(s_metrics[1].key, "temperature");
    s_metrics[1].which_value = FLOWER_METRIC_DOUBLE_VALUE_TAG;
    s_metrics[1].value.double_value = (double)(temp > -273.0f ? temp : 0);

    strcpy(s_metrics[2].key, "lux");
    s_metrics[2].which_value = FLOWER_METRIC_DOUBLE_VALUE_TAG;
    s_metrics[2].value.double_value = (double)(lux >= 0 ? lux : 0);

    int reed_closed = (gpio_get_level(REED_PIN) == 0) ? 1 : 0;
    strcpy(s_metrics[3].key, "reed");
    s_metrics[3].which_value = FLOWER_METRIC_INT64_VALUE_TAG;
    s_metrics[3].value.int64_value = reed_closed;

    strcpy(s_metrics[4].key, "mosState");
    s_metrics[4].which_value = FLOWER_METRIC_INT64_VALUE_TAG;
    s_metrics[4].value.int64_value = (int64_t)mos_current;

    strcpy(s_metrics[5].key, "mosPrevState");
    s_metrics[5].which_value = FLOWER_METRIC_INT64_VALUE_TAG;
    s_metrics[5].value.int64_value = (int64_t)mos_prev;

    strcpy(s_metrics[6].key, "aht_temp");
    s_metrics[6].which_value = FLOWER_METRIC_DOUBLE_VALUE_TAG;
    s_metrics[6].value.double_value = (aht_err == ESP_OK) ? (double)aht_temp : 0.0;

    strcpy(s_metrics[7].key, "aht_humi");
    s_metrics[7].which_value = FLOWER_METRIC_DOUBLE_VALUE_TAG;
    s_metrics[7].value.double_value = (aht_err == ESP_OK) ? (double)aht_humi : 0.0;

    s_prev_mos_state = mos_current;

    time_t now; time(&now);

    flower_status_report_t sr = FLOWER_STATUS_REPORT_INIT_ZERO;
    sr.timestamp       = (int64_t)now * 1000;
    sr.has_version     = true;
    sr.version.major   = FW_VERSION_MAJOR;
    sr.version.minor   = FW_VERSION_MINOR;
    sr.version.patch   = FW_VERSION_PATCH;
    strcpy(sr.device_type, "HT_reed_AHT25_8Xmos");
    sr.metrics.arg = s_metrics;
    sr.metrics.funcs.encode = encode_metrics;

    uint8_t buf[256];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, &flower_status_report_t_msg, &sr)) {
        ESP_LOGE(TAG, "Status encode: %s", PB_GET_ERROR(&stream));
        return;
    }
    esp_mqtt_client_publish(s_mqtt_client, s_topic_status,
                            (const char *)buf, (int)stream.bytes_written, 1, 0);

    ESP_LOGI(TAG, "Status published (humidity=%.1f%%, temp=%.2f°C, lux=%.1f, reed=%d, aht_temp=%.2f°C, aht_humi=%.1f%%, mos=0x%02lx, prev_mos=0x%02lx)",
             humidity, (double)(temp > -273.0f ? temp : 0), (double)(lux >= 0 ? lux : 0),
             reed_closed, (double)(aht_err == ESP_OK ? aht_temp : 0), (double)(aht_err == ESP_OK ? aht_humi : 0),
             (unsigned long)mos_current, (unsigned long)mos_prev);
}

/* ── Auto watering ───────────────────────────────────────────────────────── */
/* The strategy, thresholds and durations live in water_logic.c; this function
 * only reads the sensor, asks for a decision and drives the relay. */
static void check_auto_water(void)
{
    time_t now;
    time(&now);

    water_strategy_tick(now);

    if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(SENSOR_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Auto watering: sensor busy, skipping this tick");
        return;
    }
    float humidity = get_humidity();
    xSemaphoreGive(g_sensor_mutex);

    if (!water_humidity_valid(humidity)) {
        ESP_LOGW(TAG, "Auto watering: humidity %.1f%% out of range, skipping",
                 humidity);
        return;
    }

    water_decision_t d = water_strategy_eval(now, humidity);
    if (!d.water)
        return;

    if (s_relays[0].on) {
        ESP_LOGW(TAG, "Auto watering [%s]: relay 1 already on, skipping %u ms",
                 water_strategy_name(water_config()->strategy),
                 (unsigned)d.duration_ms);
        return;
    }

    ESP_LOGI(TAG, "Auto watering [%s]: humidity=%.1f%%, opening relay 1 for %u ms",
             water_strategy_name(water_config()->strategy),
             humidity, (unsigned)d.duration_ms);
    relay_set(0, true, d.duration_ms);
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
    check_auto_water();
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
static const char STATUS_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"zh\">\n"
    "<head>\n"
    "<meta charset=\"UTF-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>8路MOS控制与传感器状态</title>\n"
    "<script src=\"https://cdn.bootcdn.net/ajax/libs/Chart.js/4.4.1/chart.umd.min.js\"></script>\n"
    "<style>\n"
    "body{font-family:sans-serif;margin:0;padding:16px;background:#111;color:#eee}\n"
    "h2{margin:0 0 10px;font-size:18px}\n"
    ".vals{display:flex;gap:10px;margin-bottom:14px}\n"
    ".val{flex:1;padding:10px;background:#1e1e1e;border-radius:6px;text-align:center}\n"
    ".val .num{font-size:22px;font-weight:bold;color:#eee}\n"
    ".val .lbl{font-size:11px;color:#666;margin-top:2px}\n"
    ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:8px;margin-bottom:14px}\n"
    ".card{padding:8px 10px;background:#1e1e1e;border-radius:6px}\n"
    ".ch-hd{display:flex;justify-content:space-between;align-items:center;font-size:13px;font-weight:bold;margin-bottom:4px}\n"
    ".badge{padding:2px 6px;border-radius:4px;font-size:11px}\n"
    ".badge.on{background:#2e7d32;color:#fff}\n"
    ".badge.off{background:#424242;color:#bbb}\n"
    ".inf{font-size:11px;color:#888;margin-bottom:6px}\n"
    ".crow{display:flex;align-items:center;gap:4px}\n"
    ".crow input{width:48px;padding:4px;background:#2a2a2a;color:#eee;border:1px solid #555;border-radius:4px;text-align:center;font-size:12px}\n"
    ".crow span{font-size:11px;color:#888}\n"
    ".btn{padding:4px 10px;border:none;border-radius:4px;font-size:12px;cursor:pointer;font-weight:bold}\n"
    ".btn.on{background:#2e7d32;color:#fff;margin-left:auto}\n"
    ".btn.off{background:#c62828;color:#fff}\n"
    ".chart-wrap{margin-bottom:16px}\n"
    "canvas{background:#1a1a1a;border-radius:8px;width:100%!important}\n"
    "#status{font-size:11px;color:#555;margin-top:4px}\n"
    ".strat{margin-bottom:14px;padding:10px;background:#1e1e1e;border-radius:6px}\n"
    ".strat h3{margin:0 0 8px;font-size:14px;color:#ccc}\n"
    ".strat .cur{font-size:13px;color:#81c784;margin-bottom:8px}\n"
    ".strat .opts{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:10px}\n"
    ".strat .opts label{padding:6px 10px;background:#2a2a2a;border:1px solid #444;border-radius:6px;cursor:pointer;color:#bbb;font-size:13px}\n"
    ".strat .opts label.sel{background:#2e7d32;border-color:#2e7d32;color:#fff}\n"
    ".strat .opts input{display:none}\n"
    ".strat .row{display:flex;align-items:center;gap:6px;margin-bottom:6px;flex-wrap:wrap;font-size:13px;color:#aaa}\n"
    ".strat .row .nm{width:84px}\n"
    ".strat .row input{width:72px;padding:6px;background:#1e1e1e;color:#eee;border:1px solid #444;border-radius:6px;text-align:center;font-size:13px}\n"
    ".strat button{padding:8px 16px;background:#2e7d32;color:#fff;border:none;border-radius:6px;cursor:pointer;font-size:13px}\n"
    ".strat .msg{font-size:12px;color:#888}\n"
    ".strat .hint{font-size:11px;color:#666;margin-top:8px;line-height:1.6}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<h2>8路MOS输出控制面板<span id=\"fw-ver\" style=\"font-size:13px;color:#888;font-weight:normal;margin-left:8px\"></span></h2>\n"
    "<div class=\"grid\">\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 1 [GPIO 19]</span><span class=\"badge off\" id=\"b-1\">关</span></div><div class=\"inf\" id=\"i-1\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-1\" value=\"10\" min=\"1\" max=\"3600\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(1,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(1,'off')\">关</button></div></div>\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 2 [GPIO 18]</span><span class=\"badge off\" id=\"b-2\">关</span></div><div class=\"inf\" id=\"i-2\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-2\" value=\"10\" min=\"1\" max=\"3600\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(2,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(2,'off')\">关</button></div></div>\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 3 [GPIO 17]</span><span class=\"badge off\" id=\"b-3\">关</span></div><div class=\"inf\" id=\"i-3\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-3\" value=\"10\" min=\"1\" max=\"3600\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(3,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(3,'off')\">关</button></div></div>\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 4 [GPIO 16]</span><span class=\"badge off\" id=\"b-4\">关</span></div><div class=\"inf\" id=\"i-4\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-4\" value=\"10\" min=\"1\" max=\"3600\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(4,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(4,'off')\">关</button></div></div>\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 5 [GPIO 33]</span><span class=\"badge off\" id=\"b-5\">关</span></div><div class=\"inf\" id=\"i-5\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-5\" value=\"10\" min=\"1\" max=\"3600\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(5,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(5,'off')\">关</button></div></div>\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 6 [GPIO 25]</span><span class=\"badge off\" id=\"b-6\">关</span></div><div class=\"inf\" id=\"i-6\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-6\" value=\"10\" min=\"1\" max=\"3600\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(6,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(6,'off')\">关</button></div></div>\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 7 [GPIO 26]</span><span class=\"badge off\" id=\"b-7\">关</span></div><div class=\"inf\" id=\"i-7\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-7\" value=\"10\" min=\"1\" max=\"3600\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(7,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(7,'off')\">关</button></div></div>\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 8 [GPIO 27]</span><span class=\"badge off\" id=\"b-8\">关</span></div><div class=\"inf\" id=\"i-8\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-8\" value=\"10\" min=\"1\" max=\"3600\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(8,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(8,'off')\">关</button></div></div>\n"
    "</div>\n"
    "<h2>传感器实时数据</h2>\n"
    "<div class=\"strat\">\n"
    "<h3>自动浇水策略</h3>\n"
    "<div class=\"cur\" id=\"strat-cur\">当前：加载中...</div>\n"
    "<div class=\"opts\" id=\"strat-opts\">\n"
    "  <label data-s=\"0\"><input type=\"radio\" name=\"strat\" value=\"0\">中午12点</label>\n"
    "  <label data-s=\"1\"><input type=\"radio\" name=\"strat\" value=\"1\">浇表面</label>\n"
    "  <label data-s=\"2\"><input type=\"radio\" name=\"strat\" value=\"2\">干了再浇透</label>\n"
    "</div>\n"
    "<div class=\"row\"><span class=\"nm\">浇表面</span>湿度 &lt; <input type=\"number\" id=\"s-thresh\" min=\"0\" max=\"100\"> % → 浇 <input type=\"number\" id=\"s-dur\" min=\"1\" max=\"65535\"> 秒</div>\n"
    "<div class=\"row\"><span class=\"nm\">干了再浇透</span>湿度 &lt; <input type=\"number\" id=\"d-thresh\" min=\"0\" max=\"100\"> % → 浇 <input type=\"number\" id=\"d-dur\" min=\"1\" max=\"65535\"> 秒</div>\n"
    "<div class=\"row\" style=\"border-top:1px solid #333;padding-top:8px;margin-top:6px\"><span class=\"nm\">湿度标定</span>空气干燥(0%): <input type=\"number\" id=\"cal-dry\" min=\"0\" max=\"4095\"><button class=\"btn on\" style=\"margin-left:0;padding:4px 8px;font-size:11px\" onclick=\"setCal('dry')\">取当前AD</button>&nbsp;水中湿润(100%): <input type=\"number\" id=\"cal-wet\" min=\"0\" max=\"4095\"><button class=\"btn on\" style=\"margin-left:0;padding:4px 8px;font-size:11px\" onclick=\"setCal('wet')\">取当前AD</button></div>\n"
    "<div class=\"row\"><button onclick=\"saveWater()\">保存配置</button><span class=\"msg\" id=\"strat-msg\"></span></div>\n"
    "<div class=\"hint\">「中午12点」：每天 12:00 检查一次，阶梯式浇水 &gt;80%→6min，70-80%→8min，60-70%→10min，50-60%→15min，&lt;50%→20min（参数固定）<br>「浇表面」「干了再浇透」：<span id=\"hint-window\">08:00-16:00</span> 之间每 30 秒检查，触发后 <span id=\"hint-cooldown\">30</span> 分钟内不再自动浇水<br>「土壤湿度标定」：空气干燥值对应 0% 湿度（默认 3110），完全浸水值对应 100% 湿度（默认 958）。置于对应介质后点“取当前AD”再点击保存配置写入 Flash。</div>\n"
    "</div>\n"
    "<div class=\"vals\">\n"
    "  <div class=\"val\"><div class=\"num\" id=\"v-humi\">--</div><div class=\"lbl\">湿度 (%) <span id=\"v-raw\">(AD: --)</span></div></div>\n"
    "  <div class=\"val\"><div class=\"num\" id=\"v-temp\">--</div><div class=\"lbl\">温度 (°C)</div></div>\n"
    "  <div class=\"val\"><div class=\"num\" id=\"v-lux\">--</div><div class=\"lbl\">照度 (lx)</div></div>\n"
    "  <div class=\"val\"><div class=\"num\" id=\"v-reed\">--</div><div class=\"lbl\">干簧管 [IO14]</div></div>\n"
    "</div>\n"
    "<div class=\"chart-wrap\"><canvas id=\"c-humi\" height=\"100\"></canvas></div>\n"
    "<div class=\"chart-wrap\"><canvas id=\"c-temp\" height=\"100\"></canvas></div>\n"
    "<div class=\"chart-wrap\"><canvas id=\"c-lux\"  height=\"100\"></canvas></div>\n"
    "<div id=\"status\">正在连接...</div>\n"
    "<script>\n"
    "const p=new URLSearchParams(location.search);\n"
    "const IV=parseInt(p.get('interval')||'2000',10);\n"
    "const MX=parseInt(p.get('points')||'120',10);\n"
    "const t0=Date.now();\n"
    "const urls=['/change_relay1','/change_relay2','/change_relay3','/change_relay4','/change_relay5','/change_relay6','/change_relay7','/change_relay8'];\n"
    "function fmt(ms){const s=Math.floor(ms/1000),h=Math.floor(s/3600),m=Math.floor((s%3600)/60),ss=s%60;\n"
    "  return`${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(ss).padStart(2,'0')}`}\n"
    "function mk(id,lbl,unit,clr){\n"
    "  return new Chart(document.getElementById(id),{\n"
    "    type:'line',\n"
    "    data:{labels:[],datasets:[{label:`${lbl} (${unit})`,data:[],\n"
    "      borderColor:clr,backgroundColor:clr+'22',borderWidth:1.5,\n"
    "      pointRadius:0,tension:0.3,fill:true}]},\n"
    "    options:{animation:false,responsive:true,\n"
    "      scales:{\n"
    "        x:{ticks:{color:'#888',maxTicksLimit:8},grid:{color:'#333'}},\n"
    "        y:{ticks:{color:'#888'},grid:{color:'#333'}}},\n"
    "      plugins:{legend:{labels:{color:'#ccc'}}}}})}\n"
    "const C={h:mk('c-humi','湿度','%','#4fc3f7'),\n"
    "         t:mk('c-temp','温度','°C','#ef9a9a'),\n"
    "         l:mk('c-lux','照度','lx','#fff176')};\n"
    "function push(c,x,y){c.data.labels.push(x);c.data.datasets[0].data.push(y);\n"
    "  if(c.data.labels.length>MX){c.data.labels.shift();c.data.datasets[0].data.shift()}\n"
    "  c.update('none')}\n"
    "async function ctrl(ch,act){\n"
    "  const inp=document.getElementById('d-'+ch);\n"
    "  const sec=Math.max(1,Math.min(3600,parseInt(inp?inp.value:'10',10)||10));\n"
    "  const url=urls[ch-1]+(act==='off'?'?action=off':('?duration='+(sec*1000)));\n"
    "  try{\n"
    "    const r=await fetch(url);\n"
    "    if(!r.ok){\n"
    "      const msg=await r.text();\n"
    "      throw new Error(msg||('HTTP '+r.status));\n"
    "    }\n"
    "    poll();\n"
    "  }catch(e){\n"
    "    document.getElementById('status').textContent='控制失败: '+e.message;\n"
    "  }\n"
    "}\n"
    "function openRelay(){ctrl(1,'on');}\n"
    "async function poll(){\n"
    "  const t=fmt(Date.now()-t0);\n"
    "  try{\n"
    "    const r=await fetch('/api/sensors');\n"
    "    if(!r.ok)throw new Error('HTTP '+r.status);\n"
    "    const d=await r.json();\n"
    "    if(d.version){\n"
    "      const vEl=document.getElementById('fw-ver');\n"
    "      if(vEl)vEl.textContent='v'+d.version;\n"
    "    }\n"
    "    const hv=d.humidity.toFixed(1),tv=d.temperature.toFixed(1),lv=d.lux.toFixed(0);\n"
    "    document.getElementById('v-humi').textContent=hv;\n"
    "    const rawEl=document.getElementById('v-raw');\n"
    "    if(rawEl&&d.raw_low!==undefined){\n"
    "      latestRaw=d.raw_low;\n"
    "      rawEl.textContent=`(AD: ${d.raw_low})`;\n"
    "    }\n"
    "    document.getElementById('v-temp').textContent=tv;\n"
    "    document.getElementById('v-lux').textContent=lv;\n"
    "    const rEl=document.getElementById('v-reed');\n"
    "    if(rEl){rEl.textContent=d.reed===1?'闭合':'断开';rEl.style.color=d.reed===1?'#81c784':'#e57373';}\n"
    "    push(C.h,t,parseFloat(hv));\n"
    "    push(C.t,t,parseFloat(tv));\n"
    "    push(C.l,t,parseFloat(lv));\n"
    "    if(d.relays&&Array.isArray(d.relays)){\n"
    "      for(let i=0;i<d.relays.length;i++){\n"
    "        const item=d.relays[i];\n"
    "        const ch=item.ch||(i+1);\n"
    "        const bd=document.getElementById('b-'+ch);\n"
    "        const inf=document.getElementById('i-'+ch);\n"
    "        if(bd){\n"
    "          bd.textContent=item.state?'开':'关';\n"
    "          bd.className='badge '+(item.state?'on':'off');\n"
    "        }\n"
    "        if(inf){\n"
    "          if(item.last_on_ts>0){\n"
    "            const dt=new Date(item.last_on_ts*1000).toLocaleTimeString();\n"
    "            const dur=(item.last_on_dur_ms/1000).toFixed(0);\n"
    "            inf.textContent=`最近: ${dt} (${dur}s)`;\n"
    "          }else{inf.textContent='从未触发'}\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    if(!WC||(Number.isInteger(d.strategy)&&d.strategy!==WC.strategy))loadWater();\n"
    "    document.getElementById('status').textContent=\n"
    "      `最后更新：${new Date().toLocaleTimeString()}  轮询间隔：${IV}ms  保留点数：${MX}`;\n"
    "  }catch(e){\n"
    "    document.getElementById('status').textContent=`错误：${e.message}，重试中...`;\n"
    "  }\n"
    "}\n"
    "let WC=null,latestRaw=null;\n"
    "function setCal(which){\n"
    "  if(latestRaw===null){alert('尚未获取到传感器读数');return;}\n"
    "  if(which==='dry')document.getElementById('cal-dry').value=latestRaw;\n"
    "  if(which==='wet')document.getElementById('cal-wet').value=latestRaw;\n"
    "  markDirty();\n"
    "}\n"
    "function stratDesc(c){\n"
    "  if(c.strategy===1)return `湿度 < ${c.surface_thresh_pct}% → 浇 ${c.surface_dur_s} 秒`;\n"
    "  if(c.strategy===2)return `湿度 < ${c.deep_thresh_pct}% → 浇 ${c.deep_dur_s} 秒`;\n"
    "  return '每天 12:00 阶梯浇水';\n"
    "}\n"
    "function markStrat(id){\n"
    "  document.querySelectorAll('#strat-opts label').forEach(l=>{\n"
    "    const s=parseInt(l.dataset.s,10);\n"
    "    l.classList.toggle('sel',s===id);\n"
    "    l.querySelector('input').checked=(s===id);\n"
    "  });\n"
    "}\n"
    "function fillWater(c){\n"
    "  WC=c;\n"
    "  markStrat(c.strategy);\n"
    "  document.getElementById('s-thresh').value=c.surface_thresh_pct;\n"
    "  document.getElementById('s-dur').value=c.surface_dur_s;\n"
    "  document.getElementById('d-thresh').value=c.deep_thresh_pct;\n"
    "  document.getElementById('d-dur').value=c.deep_dur_s;\n"
    "  if(c.raw_dry!==undefined)document.getElementById('cal-dry').value=c.raw_dry;\n"
    "  if(c.raw_wet!==undefined)document.getElementById('cal-wet').value=c.raw_wet;\n"
    "  document.getElementById('hint-window').textContent=c.check_window;\n"
    "  document.getElementById('hint-cooldown').textContent=Math.round(c.cooldown_s/60);\n"
    "  document.getElementById('strat-cur').textContent=`当前：${c.strategy_name}（${stratDesc(c)}）`;\n"
    "}\n"
    "async function loadWater(){\n"
    "  try{\n"
    "    const r=await fetch('/api/water/config');\n"
    "    if(!r.ok){document.getElementById('strat-msg').textContent='读取配置失败：'+await r.text();return}\n"
    "    fillWater(await r.json());\n"
    "  }catch(e){document.getElementById('strat-msg').textContent='读取配置失败：'+e.message}\n"
    "}\n"
    "async function saveWater(){\n"
    "  const sel=document.querySelector('#strat-opts input:checked');\n"
    "  const q=new URLSearchParams({\n"
    "    strategy:sel?sel.value:'0',\n"
    "    surface_thresh_pct:document.getElementById('s-thresh').value,\n"
    "    surface_dur_s:document.getElementById('s-dur').value,\n"
    "    deep_thresh_pct:document.getElementById('d-thresh').value,\n"
    "    deep_dur_s:document.getElementById('d-dur').value,\n"
    "    raw_dry:document.getElementById('cal-dry').value,\n"
    "    raw_wet:document.getElementById('cal-wet').value});\n"
    "  const msg=document.getElementById('strat-msg');\n"
    "  msg.textContent='保存中...';\n"
    "  try{\n"
    "    const r=await fetch('/api/water/config?'+q);\n"
    "    if(!r.ok){msg.textContent='保存失败：'+await r.text();return}\n"
    "    fillWater(await r.json());\n"
    "    msg.textContent='已保存';\n"
    "  }catch(e){msg.textContent='保存失败：'+e.message}\n"
    "}\n"
    "function markDirty(){document.getElementById('strat-msg').textContent='';}\n"
    "document.querySelectorAll('#strat-opts input').forEach(i=>\n"
    "  i.addEventListener('change',()=>{markStrat(parseInt(i.value,10));markDirty();}));\n"
    "['s-thresh','s-dur','d-thresh','d-dur','cal-dry','cal-wet'].forEach(id=>\n"
    "  document.getElementById(id).addEventListener('input',markDirty));\n"
    "loadWater();\n"
    "poll();\n"
    "setInterval(poll,IV);\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

static esp_err_t handle_api_sensors(httpd_req_t *req)
{
    if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(SENSOR_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"sensor busy\"}");
        return ESP_OK;
    }

    int raw_low = adc_read_raw(ADC_PIN_LOW_HUMIDITY);
    float humidity = calculate_humidity_from_raw(raw_low);

    float temp = ds18b20_read_temperature();

    float lux = bh1750_read_illuminance();

    xSemaphoreGive(g_sensor_mutex);

    int reed_closed = (gpio_get_level(REED_PIN) == 0) ? 1 : 0;

    char json[960];
    if (s_relay_mutex) xSemaphoreTake(s_relay_mutex, portMAX_DELAY);
    snprintf(json, sizeof(json),
        "{"
        "\"version\":\"" FW_VERSION_STR "\","
        "\"humidity\":%.1f,\"raw_low\":%d,\"temperature\":%.2f,\"lux\":%.1f,\"reed\":%d,"
        "\"relay_last_on_ts\":%lld,\"relay_last_on_dur_ms\":%lu,\"strategy\":%u,"
        "\"relays\":["
        "{\"ch\":1,\"gpio\":%d,\"state\":%d,\"last_on_ts\":%lld,\"last_on_dur_ms\":%lu},"
        "{\"ch\":2,\"gpio\":%d,\"state\":%d,\"last_on_ts\":%lld,\"last_on_dur_ms\":%lu},"
        "{\"ch\":3,\"gpio\":%d,\"state\":%d,\"last_on_ts\":%lld,\"last_on_dur_ms\":%lu},"
        "{\"ch\":4,\"gpio\":%d,\"state\":%d,\"last_on_ts\":%lld,\"last_on_dur_ms\":%lu},"
        "{\"ch\":5,\"gpio\":%d,\"state\":%d,\"last_on_ts\":%lld,\"last_on_dur_ms\":%lu},"
        "{\"ch\":6,\"gpio\":%d,\"state\":%d,\"last_on_ts\":%lld,\"last_on_dur_ms\":%lu},"
        "{\"ch\":7,\"gpio\":%d,\"state\":%d,\"last_on_ts\":%lld,\"last_on_dur_ms\":%lu},"
        "{\"ch\":8,\"gpio\":%d,\"state\":%d,\"last_on_ts\":%lld,\"last_on_dur_ms\":%lu}"
        "]}",
        humidity,
        raw_low,
        (double)(temp > -273.0f ? temp : -273.0f),
        (double)(lux >= 0.0f ? lux : -1.0f),
        reed_closed,
        (long long)s_relays[0].last_on_utc_s,
        (unsigned long)s_relays[0].last_on_dur_ms,
        (unsigned)water_config()->strategy,
        s_relays[0].relay_gpio, s_relays[0].on ? 1 : 0, (long long)s_relays[0].last_on_utc_s, (unsigned long)s_relays[0].last_on_dur_ms,
        s_relays[1].relay_gpio, s_relays[1].on ? 1 : 0, (long long)s_relays[1].last_on_utc_s, (unsigned long)s_relays[1].last_on_dur_ms,
        s_relays[2].relay_gpio, s_relays[2].on ? 1 : 0, (long long)s_relays[2].last_on_utc_s, (unsigned long)s_relays[2].last_on_dur_ms,
        s_relays[3].relay_gpio, s_relays[3].on ? 1 : 0, (long long)s_relays[3].last_on_utc_s, (unsigned long)s_relays[3].last_on_dur_ms,
        s_relays[4].relay_gpio, s_relays[4].on ? 1 : 0, (long long)s_relays[4].last_on_utc_s, (unsigned long)s_relays[4].last_on_dur_ms,
        s_relays[5].relay_gpio, s_relays[5].on ? 1 : 0, (long long)s_relays[5].last_on_utc_s, (unsigned long)s_relays[5].last_on_dur_ms,
        s_relays[6].relay_gpio, s_relays[6].on ? 1 : 0, (long long)s_relays[6].last_on_utc_s, (unsigned long)s_relays[6].last_on_dur_ms,
        s_relays[7].relay_gpio, s_relays[7].on ? 1 : 0, (long long)s_relays[7].last_on_utc_s, (unsigned long)s_relays[7].last_on_dur_ms);
    if (s_relay_mutex) xSemaphoreGive(s_relay_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, STATUS_HTML, sizeof(STATUS_HTML) - 1);
    return ESP_OK;
}

static esp_err_t handle_change_relay_ch(httpd_req_t *req, int ch)
{
    if (ch < 0 || ch >= NUM_RELAYS) {
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
        if (ch == 1 && gpio_get_level(REED_PIN) != 0) {
            ESP_LOGW(TAG, "HTTP request to open MOS 2 rejected: reed switch is open");
            relay_set(2, true, 1000);
            httpd_resp_set_status(req, "403 Forbidden");
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            httpd_resp_sendstr(req, "干簧管处于断开状态，拒绝开启MOS 2（已触发MOS 3闭合1秒）");
            return ESP_OK;
        }
        relay_set(ch, true, dur);
    }

    httpd_resp_send(req, "0", 1);
    return ESP_OK;
}

static esp_err_t handle_change_relay1(httpd_req_t *req) { return handle_change_relay_ch(req, 0); }
static esp_err_t handle_change_relay2(httpd_req_t *req) { return handle_change_relay_ch(req, 1); }
static esp_err_t handle_change_relay3(httpd_req_t *req) { return handle_change_relay_ch(req, 2); }
static esp_err_t handle_change_relay4(httpd_req_t *req) { return handle_change_relay_ch(req, 3); }
static esp_err_t handle_change_relay5(httpd_req_t *req) { return handle_change_relay_ch(req, 4); }
static esp_err_t handle_change_relay6(httpd_req_t *req) { return handle_change_relay_ch(req, 5); }
static esp_err_t handle_change_relay7(httpd_req_t *req) { return handle_change_relay_ch(req, 6); }
static esp_err_t handle_change_relay8(httpd_req_t *req) { return handle_change_relay_ch(req, 7); }

/* GET /api/water/config            → current configuration as JSON
 * GET /api/water/config?strategy=1 → apply the given fields, persist, return
 *                                    the saved configuration as JSON */
static esp_err_t handle_api_water_config(httpd_req_t *req)
{
    char query[256] = {};
    esp_err_t qerr = httpd_req_get_url_query_str(req, query, sizeof(query));

    if (qerr == ESP_ERR_HTTPD_RESULT_TRUNC) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, "query too long");
        return ESP_OK;
    }

    if (qerr == ESP_OK && query[0] != '\0') {
        char err[64] = {};
        if (!water_config_apply_query(query, err, sizeof(err))) {
            ESP_LOGW(TAG, "Water config rejected: %s", err);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            httpd_resp_sendstr(req, err);
            return ESP_OK;
        }
        if (!water_store_save()) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            httpd_resp_sendstr(req, "save failed");
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Water config updated: %s", query);
    }

    char json[320];
    water_config_to_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static void http_server_start(void)
{
    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
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
    static const httpd_uri_t uri_relay5 = {
        .uri     = "/change_relay5",
        .method  = HTTP_GET,
        .handler = handle_change_relay5,
    };
    static const httpd_uri_t uri_relay6 = {
        .uri     = "/change_relay6",
        .method  = HTTP_GET,
        .handler = handle_change_relay6,
    };
    static const httpd_uri_t uri_relay7 = {
        .uri     = "/change_relay7",
        .method  = HTTP_GET,
        .handler = handle_change_relay7,
    };
    static const httpd_uri_t uri_relay8 = {
        .uri     = "/change_relay8",
        .method  = HTTP_GET,
        .handler = handle_change_relay8,
    };
    static const httpd_uri_t uri_api_sensors = {
        .uri     = "/api/sensors",
        .method  = HTTP_GET,
        .handler = handle_api_sensors,
    };
    static const httpd_uri_t uri_api_water = {
        .uri     = "/api/water/config",
        .method  = HTTP_GET,
        .handler = handle_api_water_config,
    };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_relay1);
    httpd_register_uri_handler(server, &uri_relay2);
    httpd_register_uri_handler(server, &uri_relay3);
    httpd_register_uri_handler(server, &uri_relay4);
    httpd_register_uri_handler(server, &uri_relay5);
    httpd_register_uri_handler(server, &uri_relay6);
    httpd_register_uri_handler(server, &uri_relay7);
    httpd_register_uri_handler(server, &uri_relay8);
    httpd_register_uri_handler(server, &uri_api_sensors);
    httpd_register_uri_handler(server, &uri_api_water);
    ESP_LOGI(TAG, "HTTP server started on port 80 (8 relay endpoints registered)");
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

    /* Button: active-low, toggle relay 1 on falling edge */
    bool btn = (bool)gpio_get_level(BUTTON_PIN);
    if (!btn && s_last_btn)
        relay_toggle(0);
    s_last_btn = btn;

    /* Relays auto-off */
    if (s_relay_mutex) xSemaphoreTake(s_relay_mutex, portMAX_DELAY);
    int64_t now_us = esp_timer_get_time();
    for (int ch = 0; ch < NUM_RELAYS; ch++) {
        if (s_relays[ch].on && s_relays[ch].duration_ms > 0) {
            int64_t elapsed_ms = (now_us - s_relays[ch].on_us) / 1000LL;
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

    /* Watering strategy + thresholds, restored from NVS (defaults if absent) */
    water_store_load();

    /* PSA Crypto — must be called before any psa_* API */
    ESP_ERROR_CHECK(psa_crypto_init());

    /* Factory partition — device identity (id + secret) */
    nvs_flash_init_partition("fctry");
    device_identity_init();

    /* ADC: humidity (IO0) */
    adc_init();

    /* DS18B20: temperature sensor (IO1) */
    ds18b20_init(DS18B20_PIN);

    /* I2C sensors: BH1750 (0x23) + AHT25 (0x38) on SCL=IO22, SDA=IO21 */
    i2c_sensors_init();

    /* Sensor mutex — serializes HTTP handler vs status_timer reads */
    g_sensor_mutex = xSemaphoreCreateMutex();
    assert(g_sensor_mutex != NULL);

    /* Relay mutex — serializes HTTP handlers vs timer auto-off vs auto-water */
    s_relay_mutex = xSemaphoreCreateMutex();
    assert(s_relay_mutex != NULL);

    /* GPIO: 8x MOS outputs (default off, pull-down enabled) */
    static const gpio_num_t s_mos_pins[8] = {
        MOS1_PIN, MOS2_PIN, MOS3_PIN, MOS4_PIN,
        MOS5_PIN, MOS6_PIN, MOS7_PIN, MOS8_PIN
    };
    uint64_t mos_mask = 0;
    for (int i = 0; i < 8; i++) {
        mos_mask |= (1ULL << s_mos_pins[i]);
        gpio_set_level(s_mos_pins[i], 0);  /* preset low before config so pins stay low atomically */
    }
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = mos_mask;
    io_conf.mode         = GPIO_MODE_INPUT_OUTPUT;
    io_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /* GPIO: button (input pull-up) */
    io_conf.pin_bit_mask = (1ULL << BUTTON_PIN);
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /* GPIO: reed switch (input pull-up with rising edge interrupt for closed->open transition) */
    io_conf.pin_bit_mask = (1ULL << REED_PIN);
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type    = GPIO_INTR_POSEDGE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    /* Create reed switch background task */
    xTaskCreate(reed_task, "reed_task", 2048, NULL, 5, &s_reed_task_handle);

    /* Install GPIO ISR service and attach handler for REED_PIN */
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(REED_PIN, reed_isr_handler, NULL));

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

    /* OTA task */
    s_ota_url_queue = xQueueCreate(1, OTA_URL_MAX);
    xTaskCreate(ota_task, "ota", OTA_TASK_STACK, NULL, 5, NULL);

    /* WiFi */
    wifi_init_sta();

    /* NTP (UTC+8) — needed for HMAC timestamp */
    /* Set timezone first so localtime_r is correct as soon as NTP syncs */
    setenv("TZ", "CST-8", 1);
    tzset();
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

    /* UDP Device Discovery */
    device_discovery_start(s_host_name, 80);

    /* Idle — all work is event/timer-driven */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "Uptime: %lld s", esp_timer_get_time() / 1000000LL);
    }
}
