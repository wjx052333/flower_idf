#include "aht25.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AHT25";

static i2c_master_dev_handle_t s_aht25_dev = NULL;
static bool s_initialized = false;

/* AHT25 Commands */
#define AHT25_CMD_INIT          0xBE
#define AHT25_CMD_TRIGGER       0xAC
#define AHT25_CMD_SOFT_RESET    0xBA
#define AHT25_STATUS_BUSY_MASK  0x80
#define AHT25_STATUS_CAL_MASK   0x08

esp_err_t aht25_init(i2c_master_bus_handle_t bus)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AHT25_I2C_ADDR,
        .scl_speed_hz    = 100000,
    };

    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_aht25_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add AHT25 to I2C bus: %s", esp_err_to_name(err));
        return err;
    }

    /* Wait 40ms power on stabilization */
    vTaskDelay(pdMS_TO_TICKS(40));

    /* Check status byte */
    uint8_t status = 0;
    err = i2c_master_receive(s_aht25_dev, &status, 1, 200);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AHT25 probe failed: %s", esp_err_to_name(err));
        return err;
    }

    /* If not calibrated, send init command */
    if ((status & AHT25_STATUS_CAL_MASK) == 0) {
        uint8_t init_cmd[3] = {AHT25_CMD_INIT, 0x08, 0x00};
        err = i2c_master_transmit(s_aht25_dev, init_cmd, sizeof(init_cmd), 200);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AHT25 init command failed: %s", esp_err_to_name(err));
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    s_initialized = true;
    ESP_LOGI(TAG, "AHT25 initialized successfully (status=0x%02X)", status);
    return ESP_OK;
}

esp_err_t aht25_read(float *temperature, float *humidity)
{
    if (!s_initialized || !s_aht25_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Trigger measurement command: 0xAC, 0x33, 0x00 */
    uint8_t trig_cmd[3] = {AHT25_CMD_TRIGGER, 0x33, 0x00};
    esp_err_t err = i2c_master_transmit(s_aht25_dev, trig_cmd, sizeof(trig_cmd), 200);
    if (err != ESP_OK) {
        return err;
    }

    /* Measurement takes ~75ms to 80ms */
    vTaskDelay(pdMS_TO_TICKS(80));

    /* Read 6 bytes: [0]=Status, [1]=H19..12, [2]=H11..4, [3]=H3..0|T19..16, [4]=T15..8, [5]=T7..0 */
    uint8_t data[6] = {0};
    err = i2c_master_receive(s_aht25_dev, data, sizeof(data), 200);
    if (err != ESP_OK) {
        return err;
    }

    /* Check busy bit */
    if ((data[0] & AHT25_STATUS_BUSY_MASK) != 0) {
        ESP_LOGW(TAG, "AHT25 is still busy");
        return ESP_ERR_NOT_FINISHED;
    }

    uint32_t raw_hum = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | ((uint32_t)data[3] >> 4);
    uint32_t raw_temp = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | (uint32_t)data[5];

    if (humidity) {
        *humidity = ((float)raw_hum * 100.0f) / 1048576.0f; /* 2^20 */
    }
    if (temperature) {
        *temperature = (((float)raw_temp * 200.0f) / 1048576.0f) - 50.0f;
    }

    return ESP_OK;
}
