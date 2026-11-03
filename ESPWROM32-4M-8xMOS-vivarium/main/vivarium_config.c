#include "vivarium_config.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "VivariumCfg";
static const char *NVS_NAMESPACE = "vivarium";

void vivarium_config_init_default(vivarium_config_t *cfg)
{
    if (!cfg) return;
    cfg->target_slab_temp_min     = 31.0f;
    cfg->target_slab_temp_max     = 32.2f;
    cfg->target_bulk_temp_min     = 24.0f;
    cfg->target_bulk_temp_max     = 26.5f;
    cfg->vent_humidity_thresh     = 88.0f;
    cfg->routine_vent_interval_s  = 10800; /* 3 hours */
    cfg->auto_control_enabled     = true;
    cfg->soil_raw_dry             = DEF_SOIL_RAW_DRY;
    cfg->soil_raw_wet             = DEF_SOIL_RAW_WET;
}

esp_err_t vivarium_config_load(vivarium_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    vivarium_config_init_default(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No vivarium NVS config found, using defaults");
        return ESP_OK;
    }

    size_t size = sizeof(vivarium_config_t);
    err = nvs_get_blob(h, "cfg", cfg, &size);
    nvs_close(h);

    if (err == ESP_OK && size == sizeof(vivarium_config_t)) {
        if (cfg->soil_raw_dry <= cfg->soil_raw_wet || cfg->soil_raw_dry > 4095 || cfg->soil_raw_wet > 4095) {
            ESP_LOGW(TAG, "Invalid soil calibration (dry=%u, wet=%u), resetting to defaults",
                     (unsigned)cfg->soil_raw_dry, (unsigned)cfg->soil_raw_wet);
            cfg->soil_raw_dry = DEF_SOIL_RAW_DRY;
            cfg->soil_raw_wet = DEF_SOIL_RAW_WET;
        }
        ESP_LOGI(TAG, "Vivarium config loaded from NVS (soil dry=%u wet=%u)",
                 (unsigned)cfg->soil_raw_dry, (unsigned)cfg->soil_raw_wet);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Failed to read config blob (%s), using defaults", esp_err_to_name(err));
    vivarium_config_init_default(cfg);
    return ESP_OK;
}

esp_err_t vivarium_config_save(const vivarium_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(h, "cfg", cfg, sizeof(vivarium_config_t));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Vivarium config saved to NVS");
    } else {
        ESP_LOGE(TAG, "Failed to save vivarium config: %s", esp_err_to_name(err));
    }
    return err;
}

float vivarium_calc_soil_humidity(int raw_val, uint16_t raw_dry, uint16_t raw_wet)
{
    if (raw_dry <= raw_wet) {
        raw_dry = DEF_SOIL_RAW_DRY;
        raw_wet = DEF_SOIL_RAW_WET;
    }
    float hum = (float)(raw_dry - raw_val) * 100.0f / (float)(raw_dry - raw_wet);
    if (hum < 0.0f) hum = 0.0f;
    if (hum > 100.0f) hum = 100.0f;
    return hum;
}
