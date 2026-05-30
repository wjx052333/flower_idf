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
esp_err_t camera_capture_jpeg(const uint8_t **data, uint32_t *size, bool use_flash);

/* Return the buffer to the driver (must be called after every successful capture). */
void      camera_release_jpeg(void);

/* Runtime sensor parameter adjustment (safe to call while streaming). */
esp_err_t camera_set_hmirror(int enable);   /* 0 or 1 */
esp_err_t camera_set_vflip(int enable);     /* 0 or 1 */
esp_err_t camera_set_contrast(int level);   /* -2 to +2 */
esp_err_t camera_set_saturation(int level); /* -2 to +2 */
esp_err_t camera_set_wb_mode(int mode);     /* 0=auto 1=sunny 2=cloudy 3=office 4=home */
