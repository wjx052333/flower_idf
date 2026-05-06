#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t camera_init(void);
bool      camera_is_ready(void);

/* Dequeue one JPEG frame. *data points into mmap buffer — valid until camera_release_jpeg(). */
esp_err_t camera_capture_jpeg(const uint8_t **data, uint32_t *size);

/* Return the buffer to the driver (must be called after every successful capture). */
void      camera_release_jpeg(void);
