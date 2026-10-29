#pragma once
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ds18b20_init(gpio_num_t gpio);
float ds18b20_read_temperature(void);

#ifdef __cplusplus
}
#endif