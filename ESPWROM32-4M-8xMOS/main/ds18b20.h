#pragma once
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ds18b20_init(gpio_num_t gpio);
float ds18b20_read_temperature(void);

/* Cumulative since boot, for spotting wiring / pull-up / supply problems. */
typedef struct {
    uint32_t reads;       /* ds18b20_read_temperature() calls */
    uint32_t failed;      /* calls that returned "no reading" */
    uint32_t crc_errors;  /* individual scratchpad reads rejected by CRC */
    uint32_t no_presence; /* reset got no presence pulse */
    uint32_t power_on_85; /* 85 C power-on value seen */
} ds18b20_stats_t;

void ds18b20_get_stats(ds18b20_stats_t *out);

#ifdef __cplusplus
}
#endif