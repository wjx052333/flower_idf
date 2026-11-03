#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ==============================================================================
 * Vivarium Actuator Pin & Channel Mapping (8-MOS Low-Side Switches)
 * ============================================================================== */
#define MOS_CH_BROMELIAD_PUMP    0   /* MOS 1: GPIO 19 -> 积水凤梨叶心定时补水泵 #1 */
#define MOS_CH_GECKO_PUMP        1   /* MOS 2: GPIO 18 -> 豹纹守宫定量饮水泵 #2 */
#define MOS_CH_PTC_HEATPAD       2   /* MOS 3: GPIO 17 -> 守宫热区 12V PTC 40℃ 加热片 */
#define MOS_CH_BULK_HEATPAD      3   /* MOS 4: GPIO 16 -> 干湿隔层 12V 大面积辅助加热垫 */
#define MOS_CH_ULTRASONIC_MIST   4   /* MOS 5: GPIO 33 -> 12V 微孔超声波冷雾模块电源 */
#define MOS_CH_EXHAUST_FAN       5   /* MOS 6: GPIO 25 -> 上层雨林利民 TL-C12C 抽湿风扇 */
#define MOS_CH_GROW_LIGHT        6   /* MOS 7: GPIO 26 -> 箱顶全光谱补光日照主灯 */
#define MOS_CH_SPARE             7   /* MOS 8: GPIO 27 -> 备用通用功率通道 (Spare) */

/* ==============================================================================
 * Sensors & Switches GPIO
 * ============================================================================== */
#define VIVARIUM_I2C_SDA_PIN     21  /* AHT25 (0x38) + BH1750 (0x23) */
#define VIVARIUM_I2C_SCL_PIN     22
#define VIVARIUM_DS18B20_PIN     4   /* 守宫热区页岩板贴面探头 */
#define VIVARIUM_REED_PIN        14  /* 前挡玻璃磁敏干簧管 (内部上拉) */
#define VIVARIUM_SOIL_ADC_PIN    34  /* 原土壤湿度 ADC1_CH6 (闲置保留) */

/* ==============================================================================
 * Hard Safety Limits (Firmware Safety Envelope)
 * ============================================================================== */
#define HARD_MAX_MIST_DUR_S        10     /* 雾化单次最大持续秒数 */
#define HARD_MIST_LOCKOUT_US       (15LL * 60LL * 1000000LL) /* 雾化 15 分钟锁定防抖 */

#define HARD_MAX_GECKO_PUMP_DUR_S  12     /* 守宫给水单次硬限 (秒) */
#define HARD_MAX_BROM_PUMP_DUR_S   20     /* 凤梨浇水单次硬限 (秒) */
#define HARD_MAX_BULK_HEAT_DUR_S   1800   /* 隔层辅助加热最长 30 分钟 (秒) */
#define HARD_MAX_VENT_DUR_S        90     /* 排风单次上限 (秒) */

#define HARD_CUTOFF_SLAB_TEMP      35.0f  /* 守宫岩板超温硬熔断断电阈值 (℃) */

/* ==============================================================================
 * Dynamic Configuration Stored in NVS
 * ============================================================================== */
#define DEF_SOIL_RAW_DRY  3110
#define DEF_SOIL_RAW_WET  958

typedef struct {
    /* Hotspot belly heat hysteresis */
    float    target_slab_temp_min;  /* Default: 31.0 C */
    float    target_slab_temp_max;  /* Default: 32.2 C */

    /* Bulk ambient heating hysteresis */
    float    target_bulk_temp_min;  /* Default: 24.0 C */
    float    target_bulk_temp_max;  /* Default: 26.5 C */

    /* Rainforest humidity threshold for venting */
    float    vent_humidity_thresh;  /* Default: 88.0 % */

    /* Routine fresh air purge interval */
    uint32_t routine_vent_interval_s; /* Default: 10800 s (3 hours) */

    /* Auto control enabled */
    bool     auto_control_enabled;  /* Default: true */

    /* Soil moisture 2-point ADC calibration (counts 0~4095) */
    uint16_t soil_raw_dry;          /* Default: 3110 (air / 0%) */
    uint16_t soil_raw_wet;          /* Default: 958  (water / 100%) */
} vivarium_config_t;

void vivarium_config_init_default(vivarium_config_t *cfg);
esp_err_t vivarium_config_load(vivarium_config_t *cfg);
esp_err_t vivarium_config_save(const vivarium_config_t *cfg);

/* Calculate calibrated soil humidity (0.0% - 100.0%) from raw ADC reading */
float vivarium_calc_soil_humidity(int raw_val, uint16_t raw_dry, uint16_t raw_wet);
