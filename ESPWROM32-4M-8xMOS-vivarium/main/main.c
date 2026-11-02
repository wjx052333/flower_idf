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
#include "vivarium_config.h"

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

/* ── Lux sensor select: define USE_BH1750 for BH1750FVI I2C,
 *                       undefine for Modbus-RTU sensor ─────────────── */
#define USE_BH1750

#ifndef USE_BH1750
#define MODBUS_UART_PORT  UART_NUM_1
#define MODBUS_UART_TXD   GPIO_NUM_6
#define MODBUS_UART_RXD   GPIO_NUM_7
#define MODBUS_BAUD       9600
#endif /* !USE_BH1750 */

#define STATUS_INTERVAL_MS        30000
#define SENSOR_MUTEX_TIMEOUT_MS   3000
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

    snprintf(s_host_name,      sizeof(s_host_name),      "mos_%s",                    g_device_id);
}

/* ── State ───────────────────────────────────────────────────────────────── */
static EventGroupHandle_t       s_wifi_event_group;
#define WIFI_CONNECTED_BIT      BIT0

static SemaphoreHandle_t g_sensor_mutex = NULL;

/* ── Vivarium global sensor cache (protected by g_sensor_mutex) ────────── */
static float g_air_temp   = 0.0f;  /* From AHT25 */
static float g_air_rh     = 0.0f;  /* From AHT25 */
static float g_slab_temp  = 0.0f;  /* From DS18B20 */
static float g_lux        = -1.0f; /* From BH1750 */
static float g_soil_hum   = 0.0f;  /* From ADC1_CH6 (calibrated %) */
static int   g_soil_raw   = 0;     /* Raw ADC1_CH6 counts (0~4095) */

/* ── Vivarium dynamic config (NVS persisted) ────────────────────────────── */
static vivarium_config_t s_vivarium_cfg;
static SemaphoreHandle_t s_vivarium_cfg_mutex = NULL;

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

/* ── Illuminance sensor ──────────────────────────────────────────────────── */
#ifndef USE_BH1750
/* -- Modbus-RTU path -- */
static uint16_t modbus_crc16(const uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

static void modbus_init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate = MODBUS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(MODBUS_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(MODBUS_UART_PORT, MODBUS_UART_TXD, MODBUS_UART_RXD,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(MODBUS_UART_PORT, 256, 0, 0, NULL, 0));
}

static void modbus_reset(void)
{
    uint8_t cmd[] = {0xFF, 0x06, 0x00, 0xF0, 0x00, 0x00};
    uint16_t crc = modbus_crc16(cmd, sizeof(cmd));
    uint8_t tx_buf[8];
    memcpy(tx_buf, cmd, 6);
    tx_buf[6] = crc & 0xFF;
    tx_buf[7] = (crc >> 8) & 0xFF;

    uart_flush_input(MODBUS_UART_PORT);
    uart_write_bytes(MODBUS_UART_PORT, tx_buf, 8);

    uint8_t rx_buf[8];
    int len = uart_read_bytes(MODBUS_UART_PORT, rx_buf, sizeof(rx_buf),
                              100 / portTICK_PERIOD_MS);
    if (len != 8) {
        ESP_LOGW(TAG, "Modbus reset: expected 8 bytes, got %d", len);
        return;
    }
    ESP_LOGI(TAG, "modbus_reset:0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,",
            rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4], rx_buf[5], rx_buf[6], rx_buf[7]);
    return;
}

static void modbus_read_calc_enable(void)
{
    uint8_t cmd[] = {0x01, 0x03, 0x00, 0x47, 0x00, 0x01};
    uint16_t crc = modbus_crc16(cmd, sizeof(cmd));
    uint8_t tx_buf[8];
    memcpy(tx_buf, cmd, 6);
    tx_buf[6] = crc & 0xFF;
    tx_buf[7] = (crc >> 8) & 0xFF;

    uart_flush_input(MODBUS_UART_PORT);
    uart_write_bytes(MODBUS_UART_PORT, tx_buf, 8);

    uint8_t rx_buf[7];
    int len = uart_read_bytes(MODBUS_UART_PORT, rx_buf, sizeof(rx_buf),
                              100 / portTICK_PERIOD_MS);
    if (len != 7) {
        ESP_LOGW(TAG, "modbus_read_calc: expected 7 bytes, got %d", len);
        return;
    }
    ESP_LOGI(TAG, "modbus_read_calc:0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x",
            rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4], rx_buf[5]);
    return;
}

static void modbus_read_calc(void)
{
    uint8_t cmd[] = {0x01, 0x03, 0x00, 0x48, 0x00, 0x01};
    uint16_t crc = modbus_crc16(cmd, sizeof(cmd));
    uint8_t tx_buf[8];
    memcpy(tx_buf, cmd, 6);
    tx_buf[6] = crc & 0xFF;
    tx_buf[7] = (crc >> 8) & 0xFF;

    uart_flush_input(MODBUS_UART_PORT);
    uart_write_bytes(MODBUS_UART_PORT, tx_buf, 8);

    uint8_t rx_buf[7];
    int len = uart_read_bytes(MODBUS_UART_PORT, rx_buf, sizeof(rx_buf),
                              100 / portTICK_PERIOD_MS);
    if (len != 7) {
        ESP_LOGW(TAG, "modbus_read_calc: expected 7 bytes, got %d", len);
        return;
    }
    ESP_LOGI(TAG, "modbus_read_calc:0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x",
            rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4], rx_buf[5]);
    return;
}

static float modbus_read_illuminance(void)
{
    uint8_t cmd[] = {0x01, 0x03, 0x00, 0x02, 0x00, 0x02};
    uint16_t crc = modbus_crc16(cmd, sizeof(cmd));
    uint8_t tx_buf[8];
    memcpy(tx_buf, cmd, 6);
    tx_buf[6] = crc & 0xFF;
    tx_buf[7] = (crc >> 8) & 0xFF;

    uart_flush_input(MODBUS_UART_PORT);
    uart_write_bytes(MODBUS_UART_PORT, tx_buf, 8);

    uint8_t rx_buf[9];
    int len = uart_read_bytes(MODBUS_UART_PORT, rx_buf, sizeof(rx_buf),
                              100 / portTICK_PERIOD_MS);
    if (len != 9) {
        ESP_LOGW(TAG, "Modbus illuminance: expected 9 bytes, got %d", len);
        return -1.0f;
    }

    uint32_t raw = ((uint32_t)rx_buf[3] << 24) | ((uint32_t)rx_buf[4] << 16)
                 | ((uint32_t)rx_buf[5] << 8) | rx_buf[6];
    return (float)raw / 1000.0f;
}
#else /* USE_BH1750 */
/* -- I2C Shared Bus (SCL=IO22, SDA=IO21) for BH1750 + AHT25 -- */
#define VIVARIUM_I2C_SCL_IO   GPIO_NUM_22
#define VIVARIUM_I2C_SDA_IO   GPIO_NUM_21
#define BH1750_ADDR           0x23
#define BH1750_CONT_H         0x10   /* Continuous H-Resolution Mode (~120 ms/sample) */

static i2c_master_bus_handle_t s_i2c_bus    = NULL;
static i2c_master_dev_handle_t s_bh1750_dev = NULL;

static void vivarium_i2c_sensors_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port             = I2C_NUM_0,
        .sda_io_num           = VIVARIUM_I2C_SDA_IO,
        .scl_io_num           = VIVARIUM_I2C_SCL_IO,
        .clk_source           = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt    = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    /* 1. Add BH1750 */
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

    /* 2. Add AHT25 */
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
#endif /* USE_BH1750 */

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

static flower_metric_t s_metrics[5];

static bool encode_metrics(pb_ostream_t *stream, const pb_field_t *field, void *const *arg)
{
    flower_metric_t *metrics = (flower_metric_t *)(*arg);
    for (int i = 0; i < 5; i++) {
        if (!pb_encode_tag_for_field(stream, field))
            return false;
        if (!pb_encode_submessage(stream, FLOWER_METRIC_FIELDS, &metrics[i]))
            return false;
    }
    return true;
}

static void vivarium_sample_sensors_to_cache(void)
{
    /* 1. Soil humidity from ADC1_CH6 (GPIO 34, optional/spare) */
    int raw_low = adc_read_raw(ADC_PIN_LOW_HUMIDITY);
    float humidity_cap = raw_low * 100.0f / 4096.0f;
    const float humidity_max = 98.0f;
    const float humidity_min = 30.0f;
    float soil_h = 100.0f * (humidity_max - humidity_cap) / (humidity_max - humidity_min);
    if (soil_h < 0.0f) soil_h = 0.0f;
    if (soil_h > 100.0f) soil_h = 100.0f;

    /* 2. Hotspot belly heat slab temperature from DS18B20 (GPIO 4) */
    float slab_t = ds18b20_read_temperature();

    /* 3. Rainforest air temp & humidity from AHT25 (I2C 0x38) */
    float aht_temp = 0.0f, aht_rh = 0.0f;
    bool aht_ok = (aht25_read(&aht_temp, &aht_rh) == ESP_OK);

    /* 4. Ambient illuminance from BH1750 (I2C 0x23) */
#ifdef USE_BH1750
    float lux = bh1750_read_illuminance();
#else
    float lux = -1.0f;
#endif

    /* Quick cache update with minimal mutex lock time (< 1us) */
    if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_soil_raw = raw_low;
        g_soil_hum = soil_h;
        if (slab_t > -273.0f) {
            g_slab_temp = slab_t;
        }
        if (aht_ok) {
            g_air_temp = aht_temp;
            g_air_rh   = aht_rh;
        }
        if (lux >= 0.0f) {
            g_lux = lux;
        }
        xSemaphoreGive(g_sensor_mutex);
    }
}

static void publish_status_report(void)
{
    float air_rh = 0.0f, slab_t = 0.0f, lux_val = -1.0f;
    if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        air_rh  = g_air_rh;
        slab_t  = g_slab_temp;
        lux_val = g_lux;
        xSemaphoreGive(g_sensor_mutex);
    } else {
        ESP_LOGW(TAG, "Status report: sensor cache busy, skipping this tick");
    }

    if (!s_mqtt_connected) return;

    strcpy(s_metrics[0].key, "humidity");
    s_metrics[0].which_value = FLOWER_METRIC_DOUBLE_VALUE_TAG;
    s_metrics[0].value.double_value = (double)air_rh;

    strcpy(s_metrics[1].key, "temperature");
    s_metrics[1].which_value = FLOWER_METRIC_DOUBLE_VALUE_TAG;
    s_metrics[1].value.double_value = (double)slab_t;

    strcpy(s_metrics[2].key, "lux");
    s_metrics[2].which_value = FLOWER_METRIC_DOUBLE_VALUE_TAG;
    s_metrics[2].value.double_value = (double)(lux_val >= 0 ? lux_val : 0);

    strcpy(s_metrics[3].key, "relayLastOnTs");
    s_metrics[3].which_value = FLOWER_METRIC_INT64_VALUE_TAG;
    s_metrics[3].value.int64_value = s_relays[0].last_on_utc_s;

    strcpy(s_metrics[4].key, "relayLastOnDur");
    s_metrics[4].which_value = FLOWER_METRIC_INT64_VALUE_TAG;
    s_metrics[4].value.int64_value = (int64_t)s_relays[0].last_on_dur_ms;

    time_t now; time(&now);

    flower_status_report_t sr = FLOWER_STATUS_REPORT_INIT_ZERO;
    sr.timestamp       = (int64_t)now * 1000;
    sr.has_version     = true;
    sr.version.major   = 1;
    sr.version.minor   = 1;
    sr.version.patch   = 1;
    strcpy(sr.device_type, "C3V2_humi_temp_lux_relay");
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

    ESP_LOGI(TAG, "Status published (air_rh=%.1f%%, slab_t=%.2fC, lux=%.1f)",
             air_rh, slab_t, lux_val);
}

/* ── MQTT & Timers ──────────────────────────────────────────────────────── */
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
    /* Auto watering removed in vivarium mode; autonomous control handled by vivarium_closed_loop_task */
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
    "<title>玩嘉天际S1 微型赛博生态箱控制面板</title>\n"
    "<script src=\"https://cdn.bootcdn.net/ajax/libs/Chart.js/4.4.1/chart.umd.min.js\"></script>\n"
    "<style>\n"
    "body{font-family:sans-serif;margin:0;padding:16px;background:#111;color:#eee}\n"
    "h2{margin:0 0 10px;font-size:18px;color:#81c784}\n"
    ".header{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;border-bottom:1px solid #333;padding-bottom:8px}\n"
    ".header h1{margin:0;font-size:18px;color:#81c784}\n"
    ".header .tag{font-size:12px;color:#888}\n"
    ".zone-box{background:#1a1a1a;border-radius:8px;padding:12px;margin-bottom:14px;border:1px solid #2a2a2a}\n"
    ".zone-box.upper{border-left:4px solid #4fc3f7}\n"
    ".zone-box.lower{border-left:4px solid #ffb74d}\n"
    ".zone-title{font-size:14px;font-weight:bold;margin-bottom:10px;display:flex;align-items:center;gap:6px}\n"
    ".zone-title.upper{color:#4fc3f7}\n"
    ".zone-title.lower{color:#ffb74d}\n"
    ".vals{display:flex;gap:10px;flex-wrap:wrap}\n"
    ".val{flex:1;min-width:110px;padding:10px;background:#242424;border-radius:6px;text-align:center}\n"
    ".val .num{font-size:22px;font-weight:bold;color:#eee}\n"
    ".val .lbl{font-size:11px;color:#aaa;margin-top:2px}\n"
    ".val .sub{font-size:10px;color:#666;margin-top:2px}\n"
    ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:8px;margin-bottom:14px}\n"
    ".card{padding:8px 10px;background:#1e1e1e;border-radius:6px;border:1px solid #2a2a2a}\n"
    ".ch-hd{display:flex;justify-content:space-between;align-items:center;font-size:12px;font-weight:bold;margin-bottom:4px}\n"
    ".badge{padding:2px 6px;border-radius:4px;font-size:11px}\n"
    ".badge.on{background:#2e7d32;color:#fff}\n"
    ".badge.off{background:#424242;color:#bbb}\n"
    ".inf{font-size:11px;color:#888;margin-bottom:6px}\n"
    ".crow{display:flex;align-items:center;gap:4px}\n"
    ".crow input{width:46px;padding:4px;background:#2a2a2a;color:#eee;border:1px solid #555;border-radius:4px;text-align:center;font-size:12px}\n"
    ".crow span{font-size:11px;color:#888}\n"
    ".btn{padding:4px 10px;border:none;border-radius:4px;font-size:12px;cursor:pointer;font-weight:bold}\n"
    ".btn.on{background:#2e7d32;color:#fff;margin-left:auto}\n"
    ".btn.off{background:#c62828;color:#fff}\n"
    ".chart-wrap{margin-bottom:14px}\n"
    "canvas{background:#1a1a1a;border-radius:8px;width:100%!important}\n"
    ".auto-card{background:#181818;border-radius:6px;padding:10px 12px;font-size:12px;color:#888;margin-bottom:14px;line-height:1.6;border:1px solid #282828}\n"
    ".auto-card span{color:#bbb}\n"
    "#status{font-size:11px;color:#555;margin-top:4px}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<div class=\"header\">\n"
    "  <h1>微型赛博生态箱控制面板</h1>\n"
    "  <div class=\"tag\">ESP32-WROOM-32E · 8-MOS · 群落交错区</div>\n"
    "</div>\n"
    "\n"
    "<div class=\"zone-box upper\">\n"
    "  <div class=\"zone-title upper\">🌸 上层雨林生境 (亚热带附生 · 斑眼螳螂与积水凤梨)</div>\n"
    "  <div class=\"vals\">\n"
    "    <div class=\"val\"><div class=\"num\" id=\"v-air-temp\">--</div><div class=\"lbl\">雨林空气温度</div><div class=\"sub\">舒适 25.0~28.0°C</div></div>\n"
    "    <div class=\"val\"><div class=\"num\" id=\"v-air-rh\">--</div><div class=\"lbl\">雨林空气湿度</div><div class=\"sub\">舒适 70.0~85.0%</div></div>\n"
    "    <div class=\"val\"><div class=\"num\" id=\"v-lux\">--</div><div class=\"lbl\">环境光照强度</div><div class=\"sub\">顶置日照灯</div></div>\n"
    "  </div>\n"
    "</div>\n"
    "\n"
    "<div class=\"zone-box lower\">\n"
    "  <div class=\"zone-title lower\">🦎 下层荒漠生境 (干旱地栖 · 豹纹守宫亚成体)</div>\n"
    "  <div class=\"vals\">\n"
    "    <div class=\"val\"><div class=\"num\" id=\"v-slab-temp\">--</div><div class=\"lbl\">热区石板腹热</div><div class=\"sub\">目标 31.0~32.2°C</div></div>\n"
    "    <div class=\"val\"><div class=\"num\" id=\"v-reed\">--</div><div class=\"lbl\">前挡玻璃门禁 [IO14]</div><div class=\"sub\" id=\"v-reed-sub\">磁敏防逃逸</div></div>\n"
    "    <div class=\"val\"><div class=\"num\" id=\"v-soil-hum\">--</div><div class=\"lbl\">土壤湿度 [IO34, 调试]</div><div class=\"sub\" id=\"v-soil-sub\">AD: -- / 4095</div></div>\n"
    "    <div class=\"val\"><div class=\"num\" id=\"v-fuse\">正常</div><div class=\"lbl\">生命维持硬熔断</div><div class=\"sub\">≥35.0°C 紧急切断</div></div>\n"
    "  </div>\n"
    "</div>\n"
    "\n"
    "<h2>8路 MOS 动力执行回路</h2>\n"
    "<div class=\"grid\">\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 1: 凤梨叶心补水泵</span><span class=\"badge off\" id=\"b-1\">关</span></div><div class=\"inf\" id=\"i-1\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-1\" value=\"10\" min=\"1\" max=\"20\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(1,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(1,'off')\">关</button></div></div>\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 2: 守宫饮水给水泵</span><span class=\"badge off\" id=\"b-2\">关</span></div><div class=\"inf\" id=\"i-2\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-2\" value=\"6\" min=\"1\" max=\"12\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(2,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(2,'off')\">关</button></div></div>\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 3: 守宫PTC 40℃加热</span><span class=\"badge off\" id=\"b-3\">关</span></div><div class=\"inf\" id=\"i-3\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-3\" value=\"0\" min=\"0\" max=\"3600\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(3,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(3,'off')\">关</button></div></div>\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 4: 隔层大面积加热板</span><span class=\"badge off\" id=\"b-4\">关</span></div><div class=\"inf\" id=\"i-4\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-4\" value=\"600\" min=\"1\" max=\"1800\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(4,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(4,'off')\">关</button></div></div>\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 5: 外挂超声波冷雾</span><span class=\"badge off\" id=\"b-5\">关</span></div><div class=\"inf\" id=\"i-5\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-5\" value=\"5\" min=\"1\" max=\"10\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(5,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(5,'off')\">关</button></div></div>\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 6: 雨林利民排风扇</span><span class=\"badge off\" id=\"b-6\">关</span></div><div class=\"inf\" id=\"i-6\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-6\" value=\"30\" min=\"1\" max=\"90\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(6,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(6,'off')\">关</button></div></div>\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 7: 箱顶全光谱日照灯</span><span class=\"badge off\" id=\"b-7\">关</span></div><div class=\"inf\" id=\"i-7\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-7\" value=\"3600\" min=\"0\" max=\"86400\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(7,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(7,'off')\">关</button></div></div>\n"
    "  <div class=\"card\"><div class=\"ch-hd\"><span>MOS 8: 备用通用功率通道</span><span class=\"badge off\" id=\"b-8\">关</span></div><div class=\"inf\" id=\"i-8\">从未触发</div><div class=\"crow\"><input type=\"number\" id=\"d-8\" value=\"10\" min=\"1\" max=\"3600\"><span>秒</span><button class=\"btn on\" onclick=\"ctrl(8,'on')\">开</button><button class=\"btn off\" onclick=\"ctrl(8,'off')\">关</button></div></div>\n"
    "</div>\n"
    "\n"
    "<div class=\"auto-card\">\n"
    "  <strong>🌿 本地自治生命维持策略状态</strong><br>\n"
    "  • <span>守宫热区腹热闭环</span>：目标 31.0~32.2°C (DS18B20反馈，≥35.0°C 强制硬切断)<br>\n"
    "  • <span>隔层环境底温补偿</span>：目标 24.0~26.5°C (AHT25空气温度反馈，驱动 MOS 4)<br>\n"
    "  • <span>雨林高湿主动排风</span>：湿度 &gt; 88.0% 自动强排 45s (利民风扇 MOS 6)<br>\n"
    "  • <span>定时防闷微气流置换</span>：连续 3 小时无通风则强制微风换气 30s\n"
    "</div>\n"
    "\n"
    "<h2>环境趋势曲线</h2>\n"
    "<div class=\"chart-wrap\"><canvas id=\"c-rainforest\" height=\"100\"></canvas></div>\n"
    "<div class=\"chart-wrap\"><canvas id=\"c-slab\" height=\"85\"></canvas></div>\n"
    "<div class=\"chart-wrap\"><canvas id=\"c-lux\" height=\"80\"></canvas></div>\n"
    "<div class=\"chart-wrap\">\n"
    "  <div style=\"display:flex;justify-content:space-between;align-items:center;margin-bottom:6px\">\n"
    "    <span style=\"font-size:12px;color:#81c784\">🌱 土壤湿度传感器调试分析</span>\n"
    "    <button id=\"btn-soil-mode\" class=\"btn\" style=\"background:#263238;color:#80cbc4;font-size:11px;padding:3px 10px;border:1px solid #37474f\" onclick=\"toggleSoilMode()\">切换: 原始 AD 读数</button>\n"
    "  </div>\n"
    "  <canvas id=\"c-soil\" height=\"80\"></canvas>\n"
    "</div>\n"
    "<div id=\"status\">正在连接...</div>\n"
    "\n"
    "<script>\n"
    "const p = new URLSearchParams(location.search);\n"
    "const IV = parseInt(p.get('interval') || '2000', 10);\n"
    "const MX = parseInt(p.get('points') || '120', 10);\n"
    "const t0 = Date.now();\n"
    "const urls = ['/change_relay1','/change_relay2','/change_relay3','/change_relay4','/change_relay5','/change_relay6','/change_relay7','/change_relay8'];\n"
    "\n"
    "function fmt(ms) {\n"
    "  const s = Math.floor(ms / 1000), h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), ss = s % 60;\n"
    "  return `${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(ss).padStart(2,'0')}`;\n"
    "}\n"
    "\n"
    "function mk(id, ds) {\n"
    "  return new Chart(document.getElementById(id), {\n"
    "    type: 'line',\n"
    "    data: { labels: [], datasets: ds },\n"
    "    options: {\n"
    "      animation: false,\n"
    "      responsive: true,\n"
    "      scales: {\n"
    "        x: { ticks: { color: '#888', maxTicksLimit: 8 }, grid: { color: '#333' } },\n"
    "        y: { ticks: { color: '#888' }, grid: { color: '#333' } }\n"
    "      },\n"
    "      plugins: { legend: { labels: { color: '#ccc' } } }\n"
    "    }\n"
    "  });\n"
    "}\n"
    "\n"
    "const C_rf = mk('c-rainforest', [\n"
    "  { label: '雨林空气温度 (°C)', data: [], borderColor: '#ff8a80', backgroundColor: '#ff8a8022', borderWidth: 1.5, pointRadius: 0, tension: 0.3, fill: true },\n"
    "  { label: '雨林相对湿度 (%)', data: [], borderColor: '#4fc3f7', backgroundColor: '#4fc3f722', borderWidth: 1.5, pointRadius: 0, tension: 0.3, fill: true }\n"
    "]);\n"
    "\n"
    "const C_slab = mk('c-slab', [\n"
    "  { label: '守宫石板腹热 (°C)', data: [], borderColor: '#ffb74d', backgroundColor: '#ffb74d22', borderWidth: 1.5, pointRadius: 0, tension: 0.3, fill: true }\n"
    "]);\n"
    "\n"
    "const C_lux = mk('c-lux', [\n"
    "  { label: '箱顶环境照度 (lx)', data: [], borderColor: '#fff176', backgroundColor: '#fff17622', borderWidth: 1.5, pointRadius: 0, tension: 0.3, fill: true }\n"
    "]);\n"
    "\n"
    "const C_soil = mk('c-soil', [\n"
    "  { label: '土壤相对湿度 (%)', data: [], borderColor: '#a5d6a7', backgroundColor: '#a5d6a722', borderWidth: 1.5, pointRadius: 0, tension: 0.3, fill: true }\n"
    "]);\n"
    "\n"
    "let soilMode = 'hum';\n"
    "\n"
    "function toggleSoilMode() {\n"
    "  soilMode = (soilMode === 'hum') ? 'raw' : 'hum';\n"
    "  const btn = document.getElementById('btn-soil-mode');\n"
    "  if (soilMode === 'raw') {\n"
    "    btn.textContent = '当前: 原始 AD (点此切为 %)';\n"
    "    btn.style.color = '#ffb74d';\n"
    "    C_soil.data.datasets[0].label = '土壤传感器原始 AD 采样读数 [0~4095]';\n"
    "    C_soil.data.datasets[0].borderColor = '#ffb74d';\n"
    "    C_soil.data.datasets[0].backgroundColor = '#ffb74d22';\n"
    "  } else {\n"
    "    btn.textContent = '当前: 相对湿度 % (点此切为 AD)';\n"
    "    btn.style.color = '#80cbc4';\n"
    "    C_soil.data.datasets[0].label = '土壤相对湿度 (%)';\n"
    "    C_soil.data.datasets[0].borderColor = '#a5d6a7';\n"
    "    C_soil.data.datasets[0].backgroundColor = '#a5d6a722';\n"
    "  }\n"
    "  C_soil.data.labels = [];\n"
    "  C_soil.data.datasets[0].data = [];\n"
    "  C_soil.update('none');\n"
    "}\n"
    "\n"
    "function pushMulti(chart, x, vals) {\n"
    "  chart.data.labels.push(x);\n"
    "  for (let i = 0; i < vals.length; i++) {\n"
    "    chart.data.datasets[i].data.push(vals[i]);\n"
    "  }\n"
    "  if (chart.data.labels.length > MX) {\n"
    "    chart.data.labels.shift();\n"
    "    for (let i = 0; i < vals.length; i++) {\n"
    "      chart.data.datasets[i].data.shift();\n"
    "    }\n"
    "  }\n"
    "  chart.update('none');\n"
    "}\n"
    "\n"
    "async function ctrl(ch, act) {\n"
    "  const inp = document.getElementById('d-' + ch);\n"
    "  const sec = parseInt(inp ? inp.value : '10', 10) || 0;\n"
    "  const url = urls[ch - 1] + (act === 'off' ? '?action=off' : ('?duration=' + (sec * 1000)));\n"
    "  try {\n"
    "    const r = await fetch(url);\n"
    "    if (!r.ok) throw new Error('HTTP ' + r.status);\n"
    "    poll();\n"
    "  } catch (e) {\n"
    "    document.getElementById('status').textContent = '控制失败: ' + e.message;\n"
    "  }\n"
    "}\n"
    "\n"
    "async function poll() {\n"
    "  const t = fmt(Date.now() - t0);\n"
    "  try {\n"
    "    const r = await fetch('/api/sensors');\n"
    "    if (!r.ok) throw new Error('HTTP ' + r.status);\n"
    "    const d = await r.json();\n"
    "\n"
    "    const at = (d.air_temp !== undefined ? d.air_temp : (d.temperature || 0)).toFixed(1);\n"
    "    const ar = (d.air_rh !== undefined ? d.air_rh : (d.humidity || 0)).toFixed(1);\n"
    "    const st = (d.slab_temp !== undefined ? d.slab_temp : (d.temperature || 0)).toFixed(1);\n"
    "    const lx = (d.lux !== undefined ? d.lux : 0).toFixed(0);\n"
    "    const sh = (d.soil_hum !== undefined ? d.soil_hum : 0).toFixed(1);\n"
    "    const sr = (d.soil_raw !== undefined ? d.soil_raw : '--');\n"
    "\n"
    "    document.getElementById('v-air-temp').textContent = at + ' °C';\n"
    "    document.getElementById('v-air-rh').textContent = ar + ' %';\n"
    "    document.getElementById('v-slab-temp').textContent = st + ' °C';\n"
    "    document.getElementById('v-lux').textContent = lx + ' lx';\n"
    "    document.getElementById('v-soil-hum').textContent = sh + ' %';\n"
    "    const soilSub = document.getElementById('v-soil-sub');\n"
    "    if (soilSub) soilSub.textContent = 'AD: ' + sr + ' / 4095';\n"
    "\n"
    "    const rEl = document.getElementById('v-reed');\n"
    "    const rSub = document.getElementById('v-reed-sub');\n"
    "    if (rEl) {\n"
    "      if (d.reed === 1) {\n"
    "        rEl.textContent = '闭合';\n"
    "        rEl.style.color = '#81c784';\n"
    "        if (rSub) rSub.textContent = '前挡门紧闭 (安全)';\n"
    "      } else {\n"
    "        rEl.textContent = '断开';\n"
    "        rEl.style.color = '#e57373';\n"
    "        if (rSub) rSub.textContent = '注意防逃逸！';\n"
    "      }\n"
    "    }\n"
    "\n"
    "    const fuseEl = document.getElementById('v-fuse');\n"
    "    if (fuseEl) {\n"
    "      if (parseFloat(st) >= 35.0) {\n"
    "        fuseEl.textContent = '熔断告警';\n"
    "        fuseEl.style.color = '#e57373';\n"
    "      } else {\n"
    "        fuseEl.textContent = '正常';\n"
    "        fuseEl.style.color = '#81c784';\n"
    "      }\n"
    "    }\n"
    "\n"
    "    pushMulti(C_rf, t, [parseFloat(at), parseFloat(ar)]);\n"
    "    pushMulti(C_slab, t, [parseFloat(st)]);\n"
    "    pushMulti(C_lux, t, [parseFloat(lx)]);\n"
    "    const sPlotVal = (soilMode === 'raw') ? parseInt(sr, 10) || 0 : parseFloat(sh);\n"
    "    pushMulti(C_soil, t, [sPlotVal]);\n"
    "\n"
    "    if (d.relays && Array.isArray(d.relays)) {\n"
    "      for (let i = 0; i < d.relays.length; i++) {\n"
    "        const item = d.relays[i];\n"
    "        const ch = item.ch || (i + 1);\n"
    "        const bd = document.getElementById('b-' + ch);\n"
    "        const inf = document.getElementById('i-' + ch);\n"
    "        if (bd) {\n"
    "          bd.textContent = item.state ? '开' : '关';\n"
    "          bd.className = 'badge ' + (item.state ? 'on' : 'off');\n"
    "        }\n"
    "        if (inf) {\n"
    "          if (item.last_on_ts > 0) {\n"
    "            const dt = new Date(item.last_on_ts * 1000).toLocaleTimeString();\n"
    "            const dur = (item.last_on_dur_ms / 1000).toFixed(0);\n"
    "            inf.textContent = `最近: ${dt} (${dur}s)`;\n"
    "          } else {\n"
    "            inf.textContent = '从未触发';\n"
    "          }\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    document.getElementById('status').textContent = '已连接 · 运行中 (' + t + ')';\n"
    "  } catch (e) {\n"
    "    document.getElementById('status').textContent = '连接中断: ' + e.message;\n"
    "  }\n"
    "}\n"
    "\n"
    "poll();\n"
    "setInterval(poll, IV);\n"
    "</script>\n"
    "</body>\n"
    "</html>\n"
    "\n";

static esp_err_t handle_api_sensors(httpd_req_t *req)
{
    float air_temp = 0.0f, air_rh = 0.0f, slab_temp = 0.0f, lux = -1.0f, soil_hum = 0.0f;
    int soil_raw = 0;
    if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"sensor cache busy\"}");
        return ESP_OK;
    }

    air_temp  = g_air_temp;
    air_rh    = g_air_rh;
    slab_temp = g_slab_temp;
    lux       = g_lux;
    soil_hum  = g_soil_hum;
    soil_raw  = g_soil_raw;
    xSemaphoreGive(g_sensor_mutex);

    int reed_closed = (gpio_get_level(REED_PIN) == 0) ? 1 : 0;

    char json[896];
    if (s_relay_mutex) xSemaphoreTake(s_relay_mutex, portMAX_DELAY);
    snprintf(json, sizeof(json),
        "{"
        "\"air_temp\":%.2f,\"air_rh\":%.1f,\"slab_temp\":%.2f,\"lux\":%.1f,\"soil_hum\":%.1f,\"soil_raw\":%d,\"reed\":%d,"
        "\"humidity\":%.1f,\"temperature\":%.2f,"
        "\"relay_last_on_ts\":%lld,\"relay_last_on_dur_ms\":%lu,"
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
        air_temp, air_rh, slab_temp, (double)(lux >= 0.0f ? lux : -1.0f), soil_hum, soil_raw,
        reed_closed,
        air_rh, slab_temp,
        (long long)s_relays[0].last_on_utc_s,
        (unsigned long)s_relays[0].last_on_dur_ms,
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

/* ── Vivarium Agent Endpoints ────────────────────────────────────────────── */

/* GET /api/v1/telemetry */
static esp_err_t handle_agent_telemetry(httpd_req_t *req)
{
    float air_temp = 0.0f, air_humi = 0.0f, lux = -1.0f;
    float slab_temp = 0.0f;

    if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        air_humi  = g_air_rh;
        air_temp  = g_air_temp;
        lux       = g_lux;
        slab_temp = g_slab_temp;
        xSemaphoreGive(g_sensor_mutex);
    }

    int door_closed = (gpio_get_level(REED_PIN) == 0) ? 1 : 0;

    int r_state[8] = {0};
    if (s_relay_mutex && xSemaphoreTake(s_relay_mutex, pdMS_TO_TICKS(100))) {
        for (int i = 0; i < 8; i++) {
            r_state[i] = s_relays[i].on ? 1 : 0;
        }
        xSemaphoreGive(s_relay_mutex);
    }

    char resp[1024];
    snprintf(resp, sizeof(resp),
        "{\n"
        "  \"timestamp\": %lld,\n"
        "  \"system\": {\"node_id\":\"esp32-vivarium-8mos\",\"firmware_ver\":\"1.2.0\",\"door_closed\":%s},\n"
        "  \"lower_zone\": {\n"
        "    \"habitat\": \"arid_desert\",\n"
        "    \"slab_temperature_c\": %.2f,\n"
        "    \"ptc_heater_state\": %s,\n"
        "    \"gecko_pump_state\": %s\n"
        "  },\n"
        "  \"partition_zone\": {\n"
        "    \"bulk_heater_state\": %s\n"
        "  },\n"
        "  \"upper_zone\": {\n"
        "    \"habitat\": \"subtropical_rainforest\",\n"
        "    \"air_temperature_c\": %.2f,\n"
        "    \"air_humidity_rh\": %.1f,\n"
        "    \"ambient_lux\": %.1f,\n"
        "    \"ultrasonic_mist_state\": %s,\n"
        "    \"bromeliad_pump_state\": %s,\n"
        "    \"exhaust_fan_state\": %s\n"
        "  },\n"
        "  \"raw_relays\": [%d,%d,%d,%d,%d,%d,%d,%d]\n"
        "}",
        (long long)time(NULL),
        door_closed ? "true" : "false",
        slab_temp,
        r_state[MOS_CH_PTC_HEATPAD] ? "true" : "false",
        r_state[MOS_CH_GECKO_PUMP] ? "true" : "false",
        r_state[MOS_CH_BULK_HEATPAD] ? "true" : "false",
        air_temp, air_humi, lux,
        r_state[MOS_CH_ULTRASONIC_MIST] ? "true" : "false",
        r_state[MOS_CH_BROMELIAD_PUMP] ? "true" : "false",
        r_state[MOS_CH_EXHAUST_FAN] ? "true" : "false",
        r_state[0], r_state[1], r_state[2], r_state[3],
        r_state[4], r_state[5], r_state[6], r_state[7]
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* GET /api/v1/config */
static esp_err_t handle_agent_config_get(httpd_req_t *req)
{
    char json[512];
    if (s_vivarium_cfg_mutex) xSemaphoreTake(s_vivarium_cfg_mutex, portMAX_DELAY);
    snprintf(json, sizeof(json),
        "{\n"
        "  \"target_slab_temp_min\": %.1f,\n"
        "  \"target_slab_temp_max\": %.1f,\n"
        "  \"target_bulk_temp_min\": %.1f,\n"
        "  \"target_bulk_temp_max\": %.1f,\n"
        "  \"vent_humidity_thresh\": %.1f,\n"
        "  \"routine_vent_interval_s\": %lu,\n"
        "  \"auto_control_enabled\": %s\n"
        "}",
        s_vivarium_cfg.target_slab_temp_min,
        s_vivarium_cfg.target_slab_temp_max,
        s_vivarium_cfg.target_bulk_temp_min,
        s_vivarium_cfg.target_bulk_temp_max,
        s_vivarium_cfg.vent_humidity_thresh,
        (unsigned long)s_vivarium_cfg.routine_vent_interval_s,
        s_vivarium_cfg.auto_control_enabled ? "true" : "false"
    );
    if (s_vivarium_cfg_mutex) xSemaphoreGive(s_vivarium_cfg_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

/* POST /api/v1/config (supports URL query or raw JSON body parsing) */
static esp_err_t handle_agent_config_post(httpd_req_t *req)
{
    char buf[256] = {};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) {
        buf[ret] = '\0';
    }

    /* Also check query string */
    char query[128] = {};
    httpd_req_get_url_query_str(req, query, sizeof(query));

    if (s_vivarium_cfg_mutex) xSemaphoreTake(s_vivarium_cfg_mutex, portMAX_DELAY);

    char val[32];
    if (httpd_query_key_value(query, "slab_min", val, sizeof(val)) == ESP_OK) {
        float f = (float)atof(val);
        if (f >= 25.0f && f <= 34.0f) s_vivarium_cfg.target_slab_temp_min = f;
    }
    if (httpd_query_key_value(query, "slab_max", val, sizeof(val)) == ESP_OK) {
        float f = (float)atof(val);
        if (f >= 26.0f && f <= 34.5f) s_vivarium_cfg.target_slab_temp_max = f;
    }
    if (httpd_query_key_value(query, "bulk_min", val, sizeof(val)) == ESP_OK) {
        float f = (float)atof(val);
        if (f >= 18.0f && f <= 30.0f) s_vivarium_cfg.target_bulk_temp_min = f;
    }
    if (httpd_query_key_value(query, "bulk_max", val, sizeof(val)) == ESP_OK) {
        float f = (float)atof(val);
        if (f >= 20.0f && f <= 32.0f) s_vivarium_cfg.target_bulk_temp_max = f;
    }
    if (httpd_query_key_value(query, "vent_rh", val, sizeof(val)) == ESP_OK) {
        float f = (float)atof(val);
        if (f >= 60.0f && f <= 95.0f) s_vivarium_cfg.vent_humidity_thresh = f;
    }
    if (httpd_query_key_value(query, "vent_interval_s", val, sizeof(val)) == ESP_OK) {
        uint32_t iv = (uint32_t)atoi(val);
        if (iv >= 300) s_vivarium_cfg.routine_vent_interval_s = iv;
    }
    if (httpd_query_key_value(query, "auto_ctrl", val, sizeof(val)) == ESP_OK) {
        s_vivarium_cfg.auto_control_enabled = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0);
    }

    vivarium_config_save(&s_vivarium_cfg);
    if (s_vivarium_cfg_mutex) xSemaphoreGive(s_vivarium_cfg_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// POST /api/v1/actuator/*
static int64_t s_last_mist_time_us = 0;

static esp_err_t handle_agent_actuator_dispatch(httpd_req_t *req)
{
    const char *prefix = "/api/v1/actuator/";
    if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    const char *action = req->uri + strlen(prefix);

    char query[64] = {};
    uint32_t dur_s = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16] = {};
        if (httpd_query_key_value(query, "duration_s", val, sizeof(val)) == ESP_OK) {
            dur_s = (uint32_t)atoi(val);
        }
    }

    int64_t now_us = esp_timer_get_time();

    /* A. mist */
    if (strncmp(action, "mist", 4) == 0) {
        if (s_last_mist_time_us > 0 && (now_us - s_last_mist_time_us < HARD_MIST_LOCKOUT_US)) {
            httpd_resp_set_status(req, "429 Too Many Requests");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"error\":\"misting lockout active (15min cooldown)\"}");
            return ESP_OK;
        }
        if (dur_s == 0) dur_s = 5;
        if (dur_s > HARD_MAX_MIST_DUR_S) dur_s = HARD_MAX_MIST_DUR_S;
        s_last_mist_time_us = now_us;
        relay_set(MOS_CH_ULTRASONIC_MIST, true, dur_s * 1000);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"action\":\"mist\"}");
        return ESP_OK;
    }
    /* B. gecko_water */
    else if (strncmp(action, "gecko_water", 11) == 0) {
        if (dur_s == 0) dur_s = 6;
        if (dur_s > HARD_MAX_GECKO_PUMP_DUR_S) dur_s = HARD_MAX_GECKO_PUMP_DUR_S;
        relay_set(MOS_CH_GECKO_PUMP, true, dur_s * 1000);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"action\":\"gecko_water\"}");
        return ESP_OK;
    }
    /* C. bromeliad_water */
    else if (strncmp(action, "bromeliad_water", 15) == 0) {
        if (dur_s == 0) dur_s = 10;
        if (dur_s > HARD_MAX_BROM_PUMP_DUR_S) dur_s = HARD_MAX_BROM_PUMP_DUR_S;
        relay_set(MOS_CH_BROMELIAD_PUMP, true, dur_s * 1000);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"action\":\"bromeliad_water\"}");
        return ESP_OK;
    }
    /* D. bulk_heat */
    else if (strncmp(action, "bulk_heat", 9) == 0) {
        if (dur_s == 0) dur_s = 600;
        if (dur_s > HARD_MAX_BULK_HEAT_DUR_S) dur_s = HARD_MAX_BULK_HEAT_DUR_S;
        relay_set(MOS_CH_BULK_HEATPAD, true, dur_s * 1000);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"action\":\"bulk_heat\"}");
        return ESP_OK;
    }
    /* E. vent */
    else if (strncmp(action, "vent", 4) == 0) {
        if (dur_s == 0) dur_s = 30;
        if (dur_s > HARD_MAX_VENT_DUR_S) dur_s = HARD_MAX_VENT_DUR_S;
        relay_set(MOS_CH_EXHAUST_FAN, true, dur_s * 1000);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"action\":\"vent\"}");
        return ESP_OK;
    }
    /* F. light */
    else if (strncmp(action, "light", 5) == 0) {
        char val[16] = {};
        bool is_off = (httpd_query_key_value(query, "action", val, sizeof(val)) == ESP_OK && strcmp(val, "off") == 0);
        if (is_off) {
            relay_set(MOS_CH_GROW_LIGHT, false, 0);
        } else {
            if (dur_s > 0) relay_set(MOS_CH_GROW_LIGHT, true, dur_s * 1000);
            else           relay_set(MOS_CH_GROW_LIGHT, true, 0);
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"action\":\"light\"}");
        return ESP_OK;
    }

    httpd_resp_send_404(req);
    return ESP_FAIL;
}

/* ── Autonomous Closed-Loop Climate Control Task ─────────────────────────── */
static void vivarium_closed_loop_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Vivarium autonomous climate control loop started.");
    TickType_t last_routine_vent = xTaskGetTickCount();

    /* Fault tolerance filter: keep heating on across transient DS18B20 glitches */
    static float s_last_valid_slab_t = 0.0f;
    static int   s_slab_err_cnt = 0;
    static float s_last_valid_air_t = 0.0f;
    static float s_last_valid_air_rh = 0.0f;
    static int   s_aht_err_cnt = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); /* Sample & evaluate once per second */

        /* 1. Perform hardware sampling without holding mutex */
        vivarium_sample_sensors_to_cache();

        /* 2. Read latest snapshot with quick mutex lock (< 1us) */
        float cur_slab = 0.0f, cur_air_t = 0.0f, cur_air_rh = 0.0f;
        if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            cur_slab   = g_slab_temp;
            cur_air_t  = g_air_temp;
            cur_air_rh = g_air_rh;
            xSemaphoreGive(g_sensor_mutex);
        }

        /* 3. Smooth & filter sensor readings */
        if (cur_slab > -273.0f && cur_slab < 85.0f) {
            s_last_valid_slab_t = cur_slab;
            s_slab_err_cnt = 0;
        } else {
            s_slab_err_cnt++;
        }

        if (cur_air_t > -40.0f && cur_air_t < 85.0f) {
            s_last_valid_air_t  = cur_air_t;
            s_last_valid_air_rh = cur_air_rh;
            s_aht_err_cnt = 0;
        } else {
            s_aht_err_cnt++;
        }

        float slab_t = (s_slab_err_cnt < 10) ? s_last_valid_slab_t : 0.0f;
        float air_t  = (s_aht_err_cnt < 10)  ? s_last_valid_air_t  : 0.0f;
        float air_rh = (s_aht_err_cnt < 10)  ? s_last_valid_air_rh : 0.0f;

        vivarium_config_t cfg;
        if (s_vivarium_cfg_mutex) xSemaphoreTake(s_vivarium_cfg_mutex, portMAX_DELAY);
        cfg = s_vivarium_cfg;
        if (s_vivarium_cfg_mutex) xSemaphoreGive(s_vivarium_cfg_mutex);

        if (!cfg.auto_control_enabled) {
            continue;
        }

        /* 1. Hotspot Belly Heat PTC 40℃ Control & 35℃ Emergency Cutoff */
        if (slab_t > 0.0f && slab_t < 80.0f) {
            if (slab_t >= HARD_CUTOFF_SLAB_TEMP) {
                /* Safety Emergency Cutoff */
                relay_set(MOS_CH_PTC_HEATPAD, false, 0);
                ESP_LOGE(TAG, "EMERGENCY: Hot spot slab overheated (%.1f C >= %.1f C)!",
                         slab_t, HARD_CUTOFF_SLAB_TEMP);
            } else if (slab_t < cfg.target_slab_temp_min) {
                relay_set(MOS_CH_PTC_HEATPAD, true, 0);
            } else if (slab_t >= cfg.target_slab_temp_max) {
                relay_set(MOS_CH_PTC_HEATPAD, false, 0);
            }
        } else {
            /* Sensor disconnected > 10s or invalid -> fail-safe off */
            relay_set(MOS_CH_PTC_HEATPAD, false, 0);
        }

        /* 2. Partition Bulk Ambient Heating */
        if (air_t > 0.0f) {
            if (air_t < cfg.target_bulk_temp_min) {
                relay_set(MOS_CH_BULK_HEATPAD, true, 0);
            } else if (air_t >= cfg.target_bulk_temp_max) {
                relay_set(MOS_CH_BULK_HEATPAD, false, 0);
            }
        }

        /* 3. Rainforest Excessive Humidity Venting */
        if (air_rh > cfg.vent_humidity_thresh) {
            relay_set(MOS_CH_EXHAUST_FAN, true, 45000); /* 45 seconds */
        }

        /* 4. Routine Fresh Air Purge */
        uint32_t vent_interval = (cfg.routine_vent_interval_s > 0) ? cfg.routine_vent_interval_s : 10800;
        if ((xTaskGetTickCount() - last_routine_vent) > pdMS_TO_TICKS(vent_interval * 1000ULL)) {
            relay_set(MOS_CH_EXHAUST_FAN, true, 30000); /* 30 seconds purge */
            last_routine_vent = xTaskGetTickCount();
            ESP_LOGI(TAG, "Routine fresh air purge triggered (30s)");
        }
    }
}

static void http_server_start(void)
{
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.uri_match_fn     = httpd_uri_match_wildcard; /* Enable wildcard matching */

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

    /* Vivarium Agent REST API (4 endpoints) */
    static const httpd_uri_t uri_agent_telemetry = {
        .uri     = "/api/v1/telemetry",
        .method  = HTTP_GET,
        .handler = handle_agent_telemetry,
    };
    static const httpd_uri_t uri_agent_config_get = {
        .uri     = "/api/v1/config",
        .method  = HTTP_GET,
        .handler = handle_agent_config_get,
    };
    static const httpd_uri_t uri_agent_config_post = {
        .uri     = "/api/v1/config",
        .method  = HTTP_POST,
        .handler = handle_agent_config_post,
    };
    static const httpd_uri_t uri_agent_actuator = {
        .uri     = "/api/v1/actuator/*",
        .method  = HTTP_POST,
        .handler = handle_agent_actuator_dispatch,
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

    httpd_register_uri_handler(server, &uri_agent_telemetry);
    httpd_register_uri_handler(server, &uri_agent_config_get);
    httpd_register_uri_handler(server, &uri_agent_config_post);
    httpd_register_uri_handler(server, &uri_agent_actuator);

    ESP_LOGI(TAG, "HTTP server started (14/16 handlers registered, wildcard enabled)");
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

    /* Vivarium dynamic configuration */
    s_vivarium_cfg_mutex = xSemaphoreCreateMutex();
    assert(s_vivarium_cfg_mutex != NULL);
    vivarium_config_load(&s_vivarium_cfg);

    /* PSA Crypto — must be called before any psa_* API */
    ESP_ERROR_CHECK(psa_crypto_init());

    /* Factory partition — device identity (id + secret) */
    nvs_flash_init_partition("fctry");
    device_identity_init();

    /* ADC: humidity (IO0) */
    adc_init();

    /* DS18B20: temperature sensor (IO1) */
    ds18b20_init(DS18B20_PIN);

#ifdef USE_BH1750
    /* I2C Shared Bus: BH1750 (0x23) + AHT25 (0x38) */
    vivarium_i2c_sensors_init();
#else
    /* Modbus-RTU: illuminance sensor (IO6=TXD→绿线/设备RXD, IO7=RXD→黄线/设备TXD) */
    modbus_init();
    modbus_read_calc_enable();
    modbus_read_calc(); /* modbus_reset(); */
#endif

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

    /* GPIO: reed switch (input pull-up) */
    io_conf.pin_bit_mask = (1ULL << REED_PIN);
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
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

    /* Vivarium autonomous climate control loop task (1Hz) */
    xTaskCreate(vivarium_closed_loop_task, "vivarium_loop", 4096, NULL, 4, NULL);

    /* UDP Device Discovery */
    device_discovery_start(s_host_name, 80);

    /* Idle — all work is event/timer-driven */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "Uptime: %lld s", esp_timer_get_time() / 1000000LL);
    }
}
