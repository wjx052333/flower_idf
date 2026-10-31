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
        ESP_LOGI(TAG, "Vivarium config loaded from NVS");
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
