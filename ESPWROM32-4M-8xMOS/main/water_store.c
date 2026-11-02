/**
 * water_store.c — Persist the watering configuration in NVS.
 *
 * Uses the default NVS partition, which app_main() already initialised.
 */
#include "water_store.h"

#include "esp_log.h"
#include "nvs.h"

#include "water_logic.h"

#define TAG           "WaterStore"
#define NVS_NAMESPACE "water"

void water_store_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot read water config (%s), keeping defaults",
                 esp_err_to_name(err));
        return;
    }

    /* Start from the compile-time defaults so a missing or rejected key always
     * leaves that field at its default, regardless of prior in-memory state. */
    water_config_reset();

    water_config_t *cfg = water_config();
    uint8_t  u8;
    uint16_t u16;

    if (nvs_get_u8(h, "strategy", &u8) == ESP_OK) {
        if (u8 < WATER_STRATEGY_COUNT)
            cfg->strategy = u8;
        else
            ESP_LOGW(TAG, "strategy out of range (%u), using default", (unsigned)u8);
    }
    if (nvs_get_u16(h, "s_thresh", &u16) == ESP_OK) {
        if (u16 <= WATER_PCT_MAX)
            cfg->surface_thresh_pct = u16;
        else
            ESP_LOGW(TAG, "s_thresh out of range (%u), using default", (unsigned)u16);
    }
    if (nvs_get_u16(h, "s_dur", &u16) == ESP_OK) {
        if (u16 >= WATER_DUR_MIN)
            cfg->surface_dur_s = u16;
        else
            ESP_LOGW(TAG, "s_dur out of range (%u), using default", (unsigned)u16);
    }
    if (nvs_get_u16(h, "d_thresh", &u16) == ESP_OK) {
        if (u16 <= WATER_PCT_MAX)
            cfg->deep_thresh_pct = u16;
        else
            ESP_LOGW(TAG, "d_thresh out of range (%u), using default", (unsigned)u16);
    }
    if (nvs_get_u16(h, "d_dur", &u16) == ESP_OK) {
        if (u16 >= WATER_DUR_MIN)
            cfg->deep_dur_s = u16;
        else
            ESP_LOGW(TAG, "d_dur out of range (%u), using default", (unsigned)u16);
    }
    if (nvs_get_u16(h, "raw_dry", &u16) == ESP_OK) {
        if (u16 <= WATER_RAW_ADC_MAX)
            cfg->raw_dry = u16;
        else
            ESP_LOGW(TAG, "raw_dry out of range (%u), using default", (unsigned)u16);
    }
    if (nvs_get_u16(h, "raw_wet", &u16) == ESP_OK) {
        if (u16 <= WATER_RAW_ADC_MAX)
            cfg->raw_wet = u16;
        else
            ESP_LOGW(TAG, "raw_wet out of range (%u), using default", (unsigned)u16);
    }
    if (cfg->raw_dry <= cfg->raw_wet) {
        ESP_LOGW(TAG, "raw_dry (%u) <= raw_wet (%u), resetting to defaults",
                 (unsigned)cfg->raw_dry, (unsigned)cfg->raw_wet);
        cfg->raw_dry = DEF_RAW_DRY;
        cfg->raw_wet = DEF_RAW_WET;
    }

    nvs_close(h);

    ESP_LOGI(TAG, "Water config loaded: strategy=%u surface=%u%%/%us deep=%u%%/%us dry=%u wet=%u",
             (unsigned)cfg->strategy,
             (unsigned)cfg->surface_thresh_pct, (unsigned)cfg->surface_dur_s,
             (unsigned)cfg->deep_thresh_pct, (unsigned)cfg->deep_dur_s,
             (unsigned)cfg->raw_dry, (unsigned)cfg->raw_wet);
}

bool water_store_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return false;
    }

    const water_config_t *cfg = water_config();
    err = nvs_set_u8(h, "strategy", cfg->strategy);
    if (err == ESP_OK) err = nvs_set_u16(h, "s_thresh", cfg->surface_thresh_pct);
    if (err == ESP_OK) err = nvs_set_u16(h, "s_dur",    cfg->surface_dur_s);
    if (err == ESP_OK) err = nvs_set_u16(h, "d_thresh", cfg->deep_thresh_pct);
    if (err == ESP_OK) err = nvs_set_u16(h, "d_dur",    cfg->deep_dur_s);
    if (err == ESP_OK) err = nvs_set_u16(h, "raw_dry",  cfg->raw_dry);
    if (err == ESP_OK) err = nvs_set_u16(h, "raw_wet",  cfg->raw_wet);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Water config saved");
    return true;
}
