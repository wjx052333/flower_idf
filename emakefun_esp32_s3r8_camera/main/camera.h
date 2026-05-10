#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Init camera with existing I2C bus or NULL to create one from Kconfig pins. */
esp_err_t camera_init_with_i2c(i2c_master_bus_handle_t i2c_handle);
esp_err_t camera_init(void);
bool      camera_is_ready(void);

/* Dequeue one JPEG frame. *data points into mmap buffer — valid until camera_release_jpeg(). */
esp_err_t camera_capture_jpeg(const uint8_t **data, uint32_t *size);

/* Return the buffer to the driver (must be called after every successful capture). */
void      camera_release_jpeg(void);
