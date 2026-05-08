#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/i2s_tdm.h"

/* Audio hardware config — matches waveshare ESP32-S3-CAM board */
#define AUDIO_SAMPLE_RATE      16000
#define AUDIO_MCLK_PIN         GPIO_NUM_10
#define AUDIO_BCLK_PIN         GPIO_NUM_11
#define AUDIO_WS_PIN           GPIO_NUM_12
#define AUDIO_DOUT_PIN         GPIO_NUM_14
#define AUDIO_DIN_PIN          GPIO_NUM_13
#define AUDIO_PA_PIN           GPIO_NUM_NC

#define AUDIO_INPUT_CHANNELS   4
#define AUDIO_OUTPUT_CHANNELS  1
#define AUDIO_BITS_PER_SAMPLE  16
#define AUDIO_INPUT_REFERENCE  true

/**
 * Initialize audio hardware (ES8311 speaker + ES7210 microphone array).
 * Must be called after I2C bus is initialized (same bus shared with camera).
 *
 * @param i2c_handle  I2C master bus handle (port 1, SCL=7, SDA=8)
 * @return ESP_OK on success
 */
esp_err_t audio_hw_init(i2c_master_bus_handle_t i2c_handle);

/** Enable/disable microphone input */
esp_err_t audio_input_enable(bool enable);

/** Enable/disable speaker output */
esp_err_t audio_output_enable(bool enable);

/** Read PCM samples from microphone (blocks until data available).
 *  Reads interleaved 4-channel int16_t samples.
 *  @param dest   Output buffer (must hold samples * sizeof(int16_t) bytes)
 *  @param samples Number of samples to read (total across all channels)
 *  @return number of samples read, or negative on error */
int audio_mic_read(int16_t *dest, int samples);

/** Write PCM samples to speaker.
 *  @param data    Mono int16_t samples
 *  @param samples  Number of samples
 *  @return number of samples written, or negative on error */
int audio_spk_write(const int16_t *data, int samples);

/** Set speaker volume (0-100) */
esp_err_t audio_set_volume(int volume);

/** Initialize PA control via CH32V003 IO expander (I2C addr 0x24) */
void audio_pa_init(i2c_master_bus_handle_t i2c_handle);

/** Enable/disable speaker power amplifier */
void audio_pa_enable(bool enable);

/** Deinitialize audio hardware */
void audio_hw_deinit(void);
