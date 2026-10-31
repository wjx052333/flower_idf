#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define AHT25_I2C_ADDR 0x38

/**
 * @brief Initialize AHT25 on an existing i2c_master_bus.
 *
 * @param bus Handle to initialized I2C master bus.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t aht25_init(i2c_master_bus_handle_t bus);

/**
 * @brief Trigger measurement and read temperature and relative humidity.
 *
 * @param[out] temperature Temperature in degrees Celsius.
 * @param[out] humidity Relative humidity in percent (0~100%).
 * @return esp_err_t ESP_OK if valid reading obtained.
 */
esp_err_t aht25_read(float *temperature, float *humidity);
