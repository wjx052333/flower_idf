/**
 * audio.c — ES8311 speaker + ES7210 4-mic array via esp_codec_dev
 *
 * Follows xiaozhi-esp32 BoxAudioCodec pattern:
 *   - I2S0 duplex: TDM input (4ch mic) + STD output (1ch speaker)
 *   - ES8311 DAC at 0x18, ES7210 ADC at 0x40
 */

#include <string.h>
#include "audio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "driver/i2c_master.h"
#include "driver/i2s_tdm.h"
#include "driver/i2s_std.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#define TAG "Audio"

#define DMA_DESC_NUM 8
#define DMA_FRAME_NUM 240

static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;

static const audio_codec_data_if_t *s_data_if = NULL;
static esp_codec_dev_handle_t s_spk_dev = NULL;
static esp_codec_dev_handle_t s_mic_dev = NULL;

static const audio_codec_ctrl_if_t *s_spk_ctrl = NULL;
static const audio_codec_ctrl_if_t *s_mic_ctrl = NULL;
static const audio_codec_if_t        *s_spk_codec = NULL;
static const audio_codec_if_t        *s_mic_codec = NULL;
static const audio_codec_gpio_if_t   *s_gpio_if = NULL;

static SemaphoreHandle_t s_audio_mutex = NULL;

/* ── Duplex I2S channel setup ──────────────────────────────────────────── */

static esp_err_t create_duplex(void)
{
    i2s_chan_config_t chan_cfg = {
        .id                = I2S_NUM_0,
        .role              = I2S_ROLE_MASTER,
        .dma_desc_num      = DMA_DESC_NUM,
        .dma_frame_num     = DMA_FRAME_NUM,
        .auto_clear_after_cb  = true,
        .auto_clear_before_cb = false,
        .intr_priority     = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan));

    /* TX: STD mode, 1-channel output to ES8311 */
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE,
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode      = I2S_SLOT_MODE_STEREO,
            .slot_mask      = I2S_STD_SLOT_BOTH,
            .ws_width       = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol         = false,
            .bit_shift      = true,
            .left_align     = true,
            .big_endian     = false,
            .bit_order_lsb  = false,
        },
        .gpio_cfg = {
            .mclk  = AUDIO_MCLK_PIN,
            .bclk  = AUDIO_BCLK_PIN,
            .ws    = AUDIO_WS_PIN,
            .dout  = AUDIO_DOUT_PIN,
            .din   = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));

    /* RX: TDM mode, 4-channel input from ES7210 */
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE,
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
            .bclk_div       = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode      = I2S_SLOT_MODE_STEREO,
            .slot_mask      = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                              I2S_TDM_SLOT2 | I2S_TDM_SLOT3,
            .ws_width       = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol         = false,
            .bit_shift      = true,
            .left_align     = false,
            .big_endian     = false,
            .bit_order_lsb  = false,
            .skip_mask      = false,
            .total_slot     = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk  = AUDIO_MCLK_PIN,
            .bclk  = AUDIO_BCLK_PIN,
            .ws    = AUDIO_WS_PIN,
            .dout  = I2S_GPIO_UNUSED,
            .din   = AUDIO_DIN_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(s_rx_chan, &tdm_cfg));

    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
    ESP_LOGI(TAG, "I2S duplex OK (TX:STD/%dch, RX:TDM/%dch)",
             AUDIO_OUTPUT_CHANNELS, AUDIO_INPUT_CHANNELS);
    return ESP_OK;
}

/* ── Public API ────────────────────────────────────────────────────────── */

esp_err_t audio_hw_init(i2c_master_bus_handle_t i2c_handle)
{
    s_audio_mutex = xSemaphoreCreateMutex();
    if (!s_audio_mutex) return ESP_ERR_NO_MEM;

    create_duplex();

    /* Data interface */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port     = I2S_NUM_0,
        .rx_handle = s_rx_chan,
        .tx_handle = s_tx_chan,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(s_data_if);

    /* GPIO interface */
    s_gpio_if = audio_codec_new_gpio();
    assert(s_gpio_if);

    /* ── Speaker output: ES8311 ── */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = (i2c_port_t)1,
        .addr       = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    s_spk_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(s_spk_ctrl);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if    = s_spk_ctrl;
    es8311_cfg.gpio_if    = s_gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin     = AUDIO_PA_PIN;
    es8311_cfg.use_mclk   = true;
    es8311_cfg.hw_gain.pa_voltage         = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage  = 3.3;
    s_spk_codec = es8311_codec_new(&es8311_cfg);
    assert(s_spk_codec);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_spk_codec,
        .data_if  = s_data_if,
    };
    s_spk_dev = esp_codec_dev_new(&dev_cfg);
    assert(s_spk_dev);
    ESP_LOGI(TAG, "ES8311 speaker OK (0x%02X)", ES8311_CODEC_DEFAULT_ADDR);

    /* ── Mic input: ES7210 ── */
    i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
    s_mic_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(s_mic_ctrl);

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if      = s_mic_ctrl;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 |
                              ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    s_mic_codec = es7210_codec_new(&es7210_cfg);
    assert(s_mic_codec);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = s_mic_codec;
    s_mic_dev = esp_codec_dev_new(&dev_cfg);
    assert(s_mic_dev);
    ESP_LOGI(TAG, "ES7210 mic OK (0x%02X)", ES7210_CODEC_DEFAULT_ADDR);

    ESP_LOGI(TAG, "Audio hardware initialized");
    return ESP_OK;
}

esp_err_t audio_input_enable(bool enable)
{
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    if (!s_mic_dev) {
        xSemaphoreGive(s_audio_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel         = AUDIO_INPUT_CHANNELS,
            .channel_mask    = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate     = AUDIO_SAMPLE_RATE,
        };
#if AUDIO_INPUT_REFERENCE
        fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
#endif
        esp_err_t ret = esp_codec_dev_open(s_mic_dev, &fs);
        if (ret == ESP_OK) {
            esp_codec_dev_set_in_channel_gain(s_mic_dev,
                ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), 30.0);
        }
        xSemaphoreGive(s_audio_mutex);
        return ret;
    } else {
        esp_err_t ret = esp_codec_dev_close(s_mic_dev);
        xSemaphoreGive(s_audio_mutex);
        return ret;
    }
}

esp_err_t audio_output_enable(bool enable)
{
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    if (!s_spk_dev) {
        xSemaphoreGive(s_audio_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel         = AUDIO_OUTPUT_CHANNELS,
            .channel_mask    = 0,
            .sample_rate     = AUDIO_SAMPLE_RATE,
        };
        esp_err_t ret = esp_codec_dev_open(s_spk_dev, &fs);
        xSemaphoreGive(s_audio_mutex);
        return ret;
    } else {
        esp_err_t ret = esp_codec_dev_close(s_spk_dev);
        xSemaphoreGive(s_audio_mutex);
        return ret;
    }
}

int audio_mic_read(int16_t *dest, int samples)
{
    if (!s_mic_dev) return -1;
    esp_err_t ret = esp_codec_dev_read(s_mic_dev, (void *)dest,
                                        samples * sizeof(int16_t));
    return (ret == ESP_OK) ? samples : -1;
}

int audio_spk_write(const int16_t *data, int samples)
{
    if (!s_spk_dev) return -1;
    esp_err_t ret = esp_codec_dev_write(s_spk_dev, (void *)data,
                                         samples * sizeof(int16_t));
    return (ret == ESP_OK) ? samples : -1;
}

esp_err_t audio_set_volume(int volume)
{
    if (!s_spk_dev) return ESP_ERR_INVALID_STATE;
    return esp_codec_dev_set_out_vol(s_spk_dev, volume);
}

void audio_hw_deinit(void)
{
    if (s_tx_chan) { i2s_channel_disable(s_tx_chan); i2s_del_channel(s_tx_chan); s_tx_chan = NULL; }
    if (s_rx_chan) { i2s_channel_disable(s_rx_chan); i2s_del_channel(s_rx_chan); s_rx_chan = NULL; }
    if (s_mic_dev) { esp_codec_dev_close(s_mic_dev); esp_codec_dev_delete(s_mic_dev); }
    if (s_spk_dev) { esp_codec_dev_close(s_spk_dev); esp_codec_dev_delete(s_spk_dev); }
    if (s_mic_codec) audio_codec_delete_codec_if(s_mic_codec);
    if (s_mic_ctrl)  audio_codec_delete_ctrl_if(s_mic_ctrl);
    if (s_spk_codec) audio_codec_delete_codec_if(s_spk_codec);
    if (s_spk_ctrl)  audio_codec_delete_ctrl_if(s_spk_ctrl);
    if (s_gpio_if)   audio_codec_delete_gpio_if(s_gpio_if);
    if (s_data_if)   audio_codec_delete_data_if(s_data_if);
    if (s_audio_mutex) { vSemaphoreDelete(s_audio_mutex); s_audio_mutex = NULL; }
    s_mic_dev = s_spk_dev = NULL;
}
