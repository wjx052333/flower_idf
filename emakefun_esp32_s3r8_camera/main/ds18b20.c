#include "ds18b20.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>

#define TAG "DS18B20"

#define CMD_CONVERT_T       0x44
#define CMD_READ_SCRATCHPAD 0xBE
#define CMD_SKIP_ROM        0xCC

static gpio_num_t s_gpio = GPIO_NUM_NC;

/* ── OneWire bit-banging ──────────────────────────────────────────────────── */

static inline void ow_write_bit(bool bit) {
    portDISABLE_INTERRUPTS();
    if (bit) {
        gpio_set_level(s_gpio, 0);
        esp_rom_delay_us(6);
        gpio_set_level(s_gpio, 1);
        esp_rom_delay_us(54);
    } else {
        gpio_set_level(s_gpio, 0);
        esp_rom_delay_us(60);
        gpio_set_level(s_gpio, 1);
        esp_rom_delay_us(6);
    }
    portENABLE_INTERRUPTS();
}

static inline bool ow_read_bit(void) {
    bool r;
    portDISABLE_INTERRUPTS();
    gpio_set_level(s_gpio, 0);
    esp_rom_delay_us(6);
    gpio_set_level(s_gpio, 1);
    esp_rom_delay_us(9);
    r = gpio_get_level(s_gpio);
    esp_rom_delay_us(45);
    portENABLE_INTERRUPTS();
    return r;
}

static void ow_write_byte(uint8_t v) {
    for (int i = 0; i < 8; i++) {
        ow_write_bit(v & 0x01);
        v >>= 1;
    }
}

static uint8_t ow_read_byte(void) {
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        v >>= 1;
        if (ow_read_bit()) v |= 0x80;
    }
    return v;
}

static bool ow_reset(void) {
    bool presence;
    portDISABLE_INTERRUPTS();
    gpio_set_level(s_gpio, 0);
    esp_rom_delay_us(480);
    gpio_set_level(s_gpio, 1);
    esp_rom_delay_us(70);
    presence = (gpio_get_level(s_gpio) == 0);
    esp_rom_delay_us(410);
    portENABLE_INTERRUPTS();
    return presence;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

esp_err_t ds18b20_init(gpio_num_t gpio) {
    s_gpio = gpio;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << gpio),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(%d) failed: %s", gpio, esp_err_to_name(err));
        return err;
    }
    gpio_set_level(s_gpio, 1);

    if (!ow_reset()) {
        ESP_LOGW(TAG, "No DS18B20 presence on GPIO %d", gpio);
        s_gpio = GPIO_NUM_NC;
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "DS18B20 found on GPIO %d", gpio);
    return ESP_OK;
}

float ds18b20_read_temperature(void) {
    if (s_gpio == GPIO_NUM_NC) return -273.0f;

    if (!ow_reset()) {
        ESP_LOGW(TAG, "Reset failed (start conversion)");
        return -273.0f;
    }
    ow_write_byte(CMD_SKIP_ROM);
    ow_write_byte(CMD_CONVERT_T);
    vTaskDelay(pdMS_TO_TICKS(750));

    if (!ow_reset()) {
        ESP_LOGW(TAG, "Reset failed (read scratchpad)");
        return -273.0f;
    }
    ow_write_byte(CMD_SKIP_ROM);
    ow_write_byte(CMD_READ_SCRATCHPAD);

    uint8_t lo = ow_read_byte();
    uint8_t hi = ow_read_byte();
    /* read remaining 7 bytes to complete the transaction */
    for (int i = 0; i < 7; i++) ow_read_byte();

    int16_t raw = ((int16_t)hi << 8) | lo;
    return raw / 16.0f;
}