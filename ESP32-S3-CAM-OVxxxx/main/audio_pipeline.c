/**
 * audio_pipeline.c — Unified audio subsystem (xiaozhi-esp32 dual-AFE pattern)
 *
 * Two AFE instances, never simultaneously active:
 *   IDLE:      AFE(SR) → WakeNet wake word detection
 *   LISTENING: AFE(VC) → AEC+NS+BSS → VAD → clean mono → Opus encode → callback
 *   SPEAKING:  AFE(VC) keeps running (echo cancellation), downlink Opus → decode → speaker
 *
 * Mic format: ES7210 4ch TDM → extract ch0(MIC) + ch1(REF) → AFE "MR" input
 */

#include "audio_pipeline.h"
#include "audio.h"
#include "startup_chime.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"

#include "esp_audio_enc.h"
#include "esp_audio_dec.h"
#include "encoder/impl/esp_opus_enc.h"
#include "decoder/impl/esp_opus_dec.h"

#define TAG "AudioPipe"

/* ── Constants ──────────────────────────────────────────────────────────── */

#define SAMPLE_RATE             16000
#define OPUS_FRAME_DURATION_MS  20
#define SAMPLES_PER_FRAME       ((SAMPLE_RATE * OPUS_FRAME_DURATION_MS) / 1000)  // 320
#define MAX_OPUS_PKT            256
#define PLAYBACK_QUEUE_SIZE     8
#define MIC_BUF_SIZE            (SAMPLES_PER_FRAME * 4)  /* 4ch raw */

/* Logical channels fed to AFE: 1 MIC (ch0) + 1 REF (ch1) */
#define AFE_CHANNELS            2
#define REF_CHANNEL_IDX         1

/* ── Internal types ─────────────────────────────────────────────────────── */

typedef enum {
    MODE_IDLE,
    MODE_LISTENING,
    MODE_SPEAKING,
} pipeline_mode_t;

typedef struct {
    int16_t pcm[SAMPLES_PER_FRAME];
} pcm_frame_t;

typedef struct {
    uint8_t  data[MAX_OPUS_PKT];
    uint16_t len;
} opus_pkt_t;

/* ── Static state ───────────────────────────────────────────────────────── */

static audio_pipeline_callbacks_t s_cb;

static SemaphoreHandle_t s_mutex = NULL;
static pipeline_mode_t   s_mode  = MODE_IDLE;
static volatile bool     s_running = false;

/* ── AFE(SR) — wake word ───────────────────────────────────────────────── */

static const esp_afe_sr_iface_t *s_sr_iface = NULL;
static esp_afe_sr_data_t        *s_sr_data  = NULL;
static srmodel_list_t           *s_models   = NULL;
static char                      s_wake_words[4][32];
static int                       s_wake_word_count = 0;

static int16_t *s_sr_feed_buf = NULL;
static size_t   s_sr_feed_buf_len = 0;
static size_t   s_sr_feed_chunksize = 0;

/* ── AFE(VC) — voice communication ──────────────────────────────────────── */

static const esp_afe_sr_iface_t *s_vc_iface = NULL;
static esp_afe_sr_data_t        *s_vc_data  = NULL;

static int16_t *s_vc_feed_buf = NULL;
static size_t   s_vc_feed_buf_len = 0;
static size_t   s_vc_feed_chunksize = 0;

/* Output clean mono PCM accumulator */
static int16_t *s_vc_pcm_buf = NULL;
static size_t   s_vc_pcm_buf_len = 0;

/* VAD tracking */
static bool     s_vad_speaking = false;

/* ── Opus codec ─────────────────────────────────────────────────────────── */

static void *s_opus_enc     = NULL;
static void *s_opus_dec     = NULL;
static int   s_enc_in_size  = 0;
static int   s_enc_out_size = 0;
static uint64_t s_uplink_seq = 0;

/* ── Playback ───────────────────────────────────────────────────────────── */

static QueueHandle_t s_playback_queue = NULL;

/* ── Tasks ──────────────────────────────────────────────────────────────── */

static TaskHandle_t s_mic_task_hdl     = NULL;
static TaskHandle_t s_detect_task_hdl  = NULL;
static TaskHandle_t s_process_task_hdl = NULL;
static TaskHandle_t s_playback_task_hdl = NULL;

/* Playback task stack in PSRAM to conserve internal DRAM */
#define PLAYBACK_STACK_BYTES 12288
static StackType_t *s_playback_stack = NULL;
static StaticTask_t s_playback_task_buf;

/* ── Helpers ────────────────────────────────────────────────────────────── */

static pipeline_mode_t get_mode(void)
{
    pipeline_mode_t m;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    m = s_mode;
    xSemaphoreGive(s_mutex);
    return m;
}

/* Extract 2 logical channels (MIC+REF) from 4ch TDM raw data.
 * Input:  [ch0,ch1,ch2,ch3, ch0,ch1,ch2,ch3, ...]
 * Output: [MIC,REF, MIC,REF, ...] where MIC=ch0, REF=ch1 */
static void extract_mic_ref(const int16_t *raw, int n_frames,
                            int16_t *out)
{
    for (int i = 0; i < n_frames; i++) {
        out[i * AFE_CHANNELS]     = raw[i * AUDIO_INPUT_CHANNELS];        /* ch0 → MIC */
        out[i * AFE_CHANNELS + 1] = raw[i * AUDIO_INPUT_CHANNELS + REF_CHANNEL_IDX]; /* ch1 → REF */
    }
}

/* ── Opus init ──────────────────────────────────────────────────────────── */

static esp_err_t opus_encoder_init(void)
{
    esp_opus_enc_config_t cfg = {
        .sample_rate      = SAMPLE_RATE,
        .channel          = 1,
        .bits_per_sample  = 16,
        .bitrate          = ESP_OPUS_BITRATE_AUTO,
        .frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_20_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity       = 1,
        .enable_fec       = false,
        .enable_dtx       = true,
        .enable_vbr       = true,
    };
    esp_audio_err_t ret = esp_opus_enc_open(&cfg, sizeof(cfg), &s_opus_enc);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Opus enc: %d", ret);
        return ESP_FAIL;
    }
    esp_opus_enc_get_frame_size(s_opus_enc, &s_enc_in_size, &s_enc_out_size);
    ESP_LOGI(TAG, "Opus encoder OK (in=%d out=%d)", s_enc_in_size, s_enc_out_size);
    return ESP_OK;
}

static esp_err_t opus_decoder_init(void)
{
    esp_opus_dec_cfg_t cfg = {
        .sample_rate    = SAMPLE_RATE,
        .channel        = 1,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS,
        .self_delimited = false,
    };
    esp_audio_err_t ret = esp_opus_dec_open(&cfg, sizeof(cfg), &s_opus_dec);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Opus dec: %d", ret);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Opus decoder OK");
    return ESP_OK;
}

/* ── AFE init ───────────────────────────────────────────────────────────── */

static esp_err_t afe_sr_init(void)
{
    /* Filter wake word models */
    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "esp_srmodel_init failed");
        return ESP_FAIL;
    }

    char *wn_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    ESP_LOGI(TAG, "srmodels count=%d, names:", s_models->num);
    for (int i = 0; i < s_models->num; i++)
        ESP_LOGI(TAG, "  [%d] '%s'", i, s_models->model_name[i]);

    if (!wn_name) {
        ESP_LOGE(TAG, "No WakeNet model found");
        return ESP_FAIL;
    }

    /* Parse wake words (semicolon-separated) */
    char *save = NULL;
    char *token = strtok_r(wn_name, ";", &save);
    while (token && s_wake_word_count < 4) {
        strncpy(s_wake_words[s_wake_word_count], token, 31);
        s_wake_word_count++;
        token = strtok_r(NULL, ";", &save);
    }
    ESP_LOGI(TAG, "WakeNet: %s (%d words)", wn_name, s_wake_word_count);

    /* Create AFE(SR) with AEC — format "MR" (1 MIC + 1 REF) */
    afe_config_t *cfg = afe_config_init("MR", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!cfg) {
        ESP_LOGE(TAG, "AFE(SR) config init failed");
        return ESP_FAIL;
    }
    cfg->aec_init     = true;
    cfg->aec_mode     = AEC_MODE_SR_HIGH_PERF;
    cfg->afe_perferred_core  = 1;
    cfg->afe_perferred_priority = 1;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    s_sr_iface = esp_afe_handle_from_config(cfg);
    s_sr_data  = s_sr_iface->create_from_config(cfg);
    afe_config_free(cfg);

    if (!s_sr_data) {
        ESP_LOGE(TAG, "AFE(SR) create failed");
        return ESP_FAIL;
    }

    s_sr_feed_chunksize = s_sr_iface->get_feed_chunksize(s_sr_data);
    int fetch_size = s_sr_iface->get_fetch_chunksize(s_sr_data);
    s_sr_feed_buf = heap_caps_malloc(s_sr_feed_chunksize * AFE_CHANNELS * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM);
    if (!s_sr_feed_buf) {
        ESP_LOGE(TAG, "AFE(SR) feed buf alloc failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "AFE(SR) OK: feed=%d fetch=%d", (int)s_sr_feed_chunksize, fetch_size);
    return ESP_OK;
}

static esp_err_t afe_vc_init(void)
{
    /* Load NS model */
    srmodel_list_t *vc_models = esp_srmodel_init("model");
    char *ns_name = vc_models ? esp_srmodel_filter(vc_models, ESP_NSNET_PREFIX, NULL) : NULL;

    /* Create AFE(VC) with AEC + NS + VAD — format "MR" */
    afe_config_t *cfg = afe_config_init("MR", NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    if (!cfg) {
        ESP_LOGE(TAG, "AFE(VC) config init failed");
        if (vc_models) esp_srmodel_deinit(vc_models);
        return ESP_FAIL;
    }
    cfg->aec_init       = true;
    cfg->aec_mode       = AEC_MODE_VOIP_HIGH_PERF;
    cfg->vad_init       = true;
    cfg->vad_mode       = VAD_MODE_0;
    cfg->vad_min_noise_ms = 100;
    if (ns_name) {
        cfg->ns_init       = true;
        cfg->ns_model_name = ns_name;
        ESP_LOGI(TAG, "AFE(VC) NS model: %s", ns_name);
    }
    cfg->afe_perferred_core  = 1;
    cfg->afe_perferred_priority = 1;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    s_vc_iface = esp_afe_handle_from_config(cfg);
    s_vc_data  = s_vc_iface->create_from_config(cfg);
    afe_config_free(cfg);
    if (vc_models) esp_srmodel_deinit(vc_models);

    if (!s_vc_data) {
        ESP_LOGE(TAG, "AFE(VC) create failed");
        return ESP_FAIL;
    }

    s_vc_feed_chunksize = s_vc_iface->get_feed_chunksize(s_vc_data);
    int fetch_size = s_vc_iface->get_fetch_chunksize(s_vc_data);
    s_vc_feed_buf = heap_caps_malloc(s_vc_feed_chunksize * AFE_CHANNELS * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM);
    /* pcm_buf must hold at least one AFE fetch chunk — fetch_size may exceed SAMPLES_PER_FRAME */
    size_t pcm_buf_samples = (fetch_size > SAMPLES_PER_FRAME) ? (size_t)fetch_size * 2
                                                               : (size_t)SAMPLES_PER_FRAME * 2;
    s_vc_pcm_buf = heap_caps_malloc(pcm_buf_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_vc_pcm_buf_len = 0;
    if (fetch_size > SAMPLES_PER_FRAME)
        ESP_LOGW(TAG, "AFE(VC) fetch=%d > SAMPLES_PER_FRAME=%d, pcm_buf sized to %d",
                 fetch_size, SAMPLES_PER_FRAME, (int)pcm_buf_samples);
    if (!s_vc_feed_buf || !s_vc_pcm_buf) {
        ESP_LOGE(TAG, "AFE(VC) buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "AFE(VC) OK: feed=%d fetch=%d", (int)s_vc_feed_chunksize, fetch_size);
    return ESP_OK;
}

/* ── Mic feed task ──────────────────────────────────────────────────────── */

static void mic_feed_task(void *arg)
{
    int16_t raw[MIC_BUF_SIZE];  /* 4ch raw read buffer */

    ESP_LOGI(TAG, "Mic feed task started");
    audio_input_enable(true);

    while (s_running) {
        pipeline_mode_t mode = get_mode();
        int read = audio_mic_read(raw, SAMPLES_PER_FRAME * AUDIO_INPUT_CHANNELS);
        if (read < 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (mode == MODE_IDLE) {
            /* Feed AFE(SR) for wake word */
            if (s_sr_feed_buf_len + SAMPLES_PER_FRAME > s_sr_feed_chunksize * AFE_CHANNELS)
                s_sr_feed_buf_len = 0;  /* overflow safety */

            extract_mic_ref(raw, SAMPLES_PER_FRAME,
                            s_sr_feed_buf + s_sr_feed_buf_len);
            s_sr_feed_buf_len += SAMPLES_PER_FRAME * AFE_CHANNELS;

            while (s_sr_feed_buf_len >= s_sr_feed_chunksize * AFE_CHANNELS) {
                s_sr_iface->feed(s_sr_data, s_sr_feed_buf);
                size_t consumed = s_sr_feed_chunksize * AFE_CHANNELS;
                memmove(s_sr_feed_buf, s_sr_feed_buf + consumed,
                        (s_sr_feed_buf_len - consumed) * sizeof(int16_t));
                s_sr_feed_buf_len -= consumed;
            }
        } else {
            /* MODE_LISTENING or MODE_SPEAKING: feed AFE(VC) */
            if (s_vc_feed_buf_len + SAMPLES_PER_FRAME > s_vc_feed_chunksize * AFE_CHANNELS)
                s_vc_feed_buf_len = 0;

            extract_mic_ref(raw, SAMPLES_PER_FRAME,
                            s_vc_feed_buf + s_vc_feed_buf_len);
            s_vc_feed_buf_len += SAMPLES_PER_FRAME * AFE_CHANNELS;

            while (s_vc_feed_buf_len >= s_vc_feed_chunksize * AFE_CHANNELS) {
                s_vc_iface->feed(s_vc_data, s_vc_feed_buf);
                size_t consumed = s_vc_feed_chunksize * AFE_CHANNELS;
                memmove(s_vc_feed_buf, s_vc_feed_buf + consumed,
                        (s_vc_feed_buf_len - consumed) * sizeof(int16_t));
                s_vc_feed_buf_len -= consumed;
            }
        }

        taskYIELD();
    }

    audio_input_enable(false);
    ESP_LOGI(TAG, "Mic feed task exiting");
    vTaskDelete(NULL);
}

/* ── AFE(SR) detect task ────────────────────────────────────────────────── */

static void detect_task(void *arg)
{
    ESP_LOGI(TAG, "Detect task started");

    while (s_running) {
        if (get_mode() != MODE_IDLE) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        afe_fetch_result_t *res = s_sr_iface->fetch(s_sr_data);
        if (!res || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            int idx = res->wake_word_index;
            const char *word = (idx >= 0 && idx < s_wake_word_count)
                               ? s_wake_words[idx] : "unknown";
            ESP_LOGI(TAG, "Wake word: %s (idx=%d)", word, idx);
            if (s_cb.on_wake_word)
                s_cb.on_wake_word(s_cb.user_data);
        }

        taskYIELD();
    }

    ESP_LOGI(TAG, "Detect task exiting");
    vTaskDelete(NULL);
}

/* ── AFE(VC) process task ───────────────────────────────────────────────── */

static void process_task(void *arg)
{
    uint8_t *enc_out = malloc(s_enc_out_size);
    if (!enc_out) {
        ESP_LOGE(TAG, "Process task: enc_out alloc failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Process task started");

    while (s_running) {
        pipeline_mode_t mode = get_mode();
        if (mode != MODE_LISTENING && mode != MODE_SPEAKING) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        afe_fetch_result_t *res = s_vc_iface->fetch(s_vc_data);
        if (!res || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* VAD state change */
        bool speaking = (res->vad_state == VAD_SPEECH);
        if (speaking != s_vad_speaking) {
            s_vad_speaking = speaking;
            if (s_cb.on_vad_change)
                s_cb.on_vad_change(speaking, s_cb.user_data);
            ESP_LOGD(TAG, "VAD: %s", speaking ? "SPEECH" : "SILENCE");
        }

        /* Accumulate clean mono PCM from AFE output */
        int fetch_samples = res->data_size / sizeof(int16_t);
        if (s_vc_pcm_buf_len + fetch_samples > SAMPLES_PER_FRAME) {
            s_vc_pcm_buf_len = 0;  /* safety reset */
        }
        memcpy(s_vc_pcm_buf + s_vc_pcm_buf_len, res->data,
               fetch_samples * sizeof(int16_t));
        s_vc_pcm_buf_len += fetch_samples;

        /* Encode when we have enough for one Opus frame */
        while (s_vc_pcm_buf_len >= SAMPLES_PER_FRAME && mode == MODE_LISTENING) {
            esp_audio_enc_in_frame_t in = {
                .buffer = (uint8_t *)s_vc_pcm_buf,
                .len    = (uint32_t)s_enc_in_size,
            };
            esp_audio_enc_out_frame_t out = {
                .buffer = enc_out,
                .len    = (uint32_t)s_enc_out_size,
            };
            esp_audio_err_t ret = esp_opus_enc_process(s_opus_enc, &in, &out);
            if (ret == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0) {
                if (s_cb.on_uplink_opus) {
                    s_cb.on_uplink_opus(enc_out, out.encoded_bytes,
                                        s_uplink_seq++,
                                        esp_timer_get_time() / 1000,
                                        false, s_cb.user_data);
                }
            }

            /* Consume SAMPLES_PER_FRAME from accumulator */
            size_t remain = s_vc_pcm_buf_len - SAMPLES_PER_FRAME;
            if (remain > 0)
                memmove(s_vc_pcm_buf, s_vc_pcm_buf + SAMPLES_PER_FRAME,
                        remain * sizeof(int16_t));
            s_vc_pcm_buf_len = remain;

            /* Re-read mode in case it changed during encoding */
            mode = get_mode();
        }

        taskYIELD();
    }

    free(enc_out);
    ESP_LOGI(TAG, "Process task exiting");
    vTaskDelete(NULL);
}

/* ── Playback task ──────────────────────────────────────────────────────── */

static void playback_task(void *arg)
{
    opus_pkt_t pkt;
    ESP_LOGI(TAG, "Playback task started");
    audio_output_enable(true);
    audio_set_volume(80);

    /* Enable speaker PA via IO expander — required for sound on this board */
    audio_pa_enable(true);

    /* Startup chime — validates speaker chain (I2S TX → ES8311 → PA → speaker) */
    ESP_LOGI(TAG, "Startup chime: %d samples (%.0f ms)", STARTUP_CHIME_NUM_SAMPLES,
             (STARTUP_CHIME_NUM_SAMPLES * 1000) / SAMPLE_RATE);
    for (int i = 0; i < STARTUP_CHIME_NUM_SAMPLES; i += SAMPLES_PER_FRAME) {
        int chunk = STARTUP_CHIME_NUM_SAMPLES - i;
        if (chunk > SAMPLES_PER_FRAME) chunk = SAMPLES_PER_FRAME;
        int wr = audio_spk_write(startup_chime_pcm + i, chunk);
        if (wr < 0) ESP_LOGW(TAG, "Chime write @%d: %d", i, wr);
    }
    ESP_LOGI(TAG, "Startup chime done");

    while (s_running) {
        if (xQueueReceive(s_playback_queue, &pkt, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;

        esp_audio_dec_in_raw_t raw = {
            .buffer        = pkt.data,
            .len           = (uint32_t)pkt.len,
            .consumed      = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        pcm_frame_t pcm;
        esp_audio_dec_out_frame_t out = {
            .buffer       = (uint8_t *)pcm.pcm,
            .len          = sizeof(pcm.pcm),
            .decoded_size = 0,
        };
        esp_audio_dec_info_t dec_info = {};
        esp_audio_err_t ret = esp_opus_dec_decode(s_opus_dec, &raw, &out, &dec_info);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "Opus decode: %d", ret);
            continue;
        }
        if (out.decoded_size > 0) {
            ESP_LOGD(TAG, "Opus → %u B PCM (%u Hz %u ch)",
                     (unsigned)out.decoded_size, (unsigned)dec_info.sample_rate,
                     (unsigned)dec_info.channel);
            int wr = audio_spk_write(pcm.pcm, SAMPLES_PER_FRAME);
            if (wr < 0) ESP_LOGW(TAG, "Speaker write: %d", wr);
        }
    }

    ESP_LOGI(TAG, "Playback task exiting");
    vTaskDelete(NULL);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t audio_pipeline_init(const audio_pipeline_callbacks_t *callbacks)
{
    if (!callbacks || !callbacks->on_wake_word || !callbacks->on_uplink_opus) {
        ESP_LOGE(TAG, "on_wake_word and on_uplink_opus callbacks required");
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(&s_cb, callbacks, sizeof(s_cb));

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_playback_queue = xQueueCreate(PLAYBACK_QUEUE_SIZE, sizeof(opus_pkt_t));
    if (!s_playback_queue) return ESP_ERR_NO_MEM;

    /* Init AFE instances */
    esp_err_t ret = afe_sr_init();
    if (ret != ESP_OK) return ret;

    ret = afe_vc_init();
    if (ret != ESP_OK) return ret;

    /* Init Opus */
    ret = opus_encoder_init();
    if (ret != ESP_OK) return ret;

    ret = opus_decoder_init();
    if (ret != ESP_OK) return ret;

    /* Start background tasks */
    s_running = true;
    xTaskCreatePinnedToCore(mic_feed_task, "mic_feed", 6144, NULL, 6,
                            &s_mic_task_hdl, 0);
    xTaskCreatePinnedToCore(detect_task,  "afe_det",  3072, NULL, 4,
                            &s_detect_task_hdl, 1);
    xTaskCreatePinnedToCore(process_task, "afe_proc", 3072, NULL, 4,
                            &s_process_task_hdl, 1);

    /* Playback task stack allocated from PSRAM to conserve internal DRAM */
    s_playback_stack = heap_caps_malloc(PLAYBACK_STACK_BYTES, MALLOC_CAP_SPIRAM);
    if (s_playback_stack) {
        s_playback_task_hdl = xTaskCreateStaticPinnedToCore(
            playback_task, "audio_play", PLAYBACK_STACK_BYTES,
            NULL, 5, s_playback_stack, &s_playback_task_buf, 1);
    }
    if (!s_playback_task_hdl) {
        ESP_LOGE(TAG, "Playback task create failed");
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Audio pipeline initialized (SR+VC dual AFE, 16kHz, AEC+NS+VAD)");
    return ESP_OK;
}

esp_err_t audio_pipeline_start_listening(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_mode != MODE_IDLE) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Not in IDLE mode, cannot start listening");
        return ESP_ERR_INVALID_STATE;
    }

    /* Clear AFE(VC) buffer for clean start */
    s_vc_iface->reset_buffer(s_vc_data);
    s_vc_feed_buf_len = 0;
    s_vc_pcm_buf_len = 0;
    s_vad_speaking   = false;
    s_uplink_seq     = 0;

    s_mode = MODE_LISTENING;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Mode: IDLE → LISTENING");
    return ESP_OK;
}

esp_err_t audio_pipeline_stop_listening(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_mode == MODE_IDLE) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    /* Send EOS */
    if (s_cb.on_uplink_opus) {
        s_cb.on_uplink_opus(NULL, 0, s_uplink_seq++,
                            esp_timer_get_time() / 1000, true, s_cb.user_data);
    }

    /* Reset AFE(VC), clear SR feed for clean restart */
    s_vc_iface->reset_buffer(s_vc_data);
    s_vc_feed_buf_len = 0;
    s_vc_pcm_buf_len = 0;
    s_sr_feed_buf_len = 0;

    s_mode = MODE_IDLE;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Mode: → IDLE");
    return ESP_OK;
}

esp_err_t audio_pipeline_feed_downlink(const uint8_t *opus_data, size_t len,
                                        bool is_eos)
{
    if (!s_mutex || !s_opus_dec) return ESP_ERR_INVALID_STATE;

    if (is_eos) {
        bool was_speaking = false;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_mode == MODE_SPEAKING) {
            s_mode = MODE_LISTENING;
            was_speaking = true;
            ESP_LOGI(TAG, "Downlink EOS → LISTENING");
        }
        xSemaphoreGive(s_mutex);
        if (was_speaking && s_cb.on_downlink_eos)
            s_cb.on_downlink_eos(s_cb.user_data);
        return ESP_OK;
    }

    /* Stats-only messages have no opus_data — skip silently */
    if (!opus_data || len == 0) return ESP_OK;

    ESP_LOGD(TAG, "Decode downlink: len=%u", (unsigned)len);

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_mode == MODE_LISTENING) {
        s_mode = MODE_SPEAKING;
        ESP_LOGI(TAG, "Mode: LISTENING → SPEAKING (TTS playback)");
    }
    xSemaphoreGive(s_mutex);

    opus_pkt_t pkt = { .len = (uint16_t)len };
    if (len > MAX_OPUS_PKT) pkt.len = MAX_OPUS_PKT;
    memcpy(pkt.data, opus_data, pkt.len);
    if (xQueueSend(s_playback_queue, &pkt, 0) != pdTRUE)
        ESP_LOGW(TAG, "Playback queue full, dropping frame");
    return ESP_OK;
}

bool audio_pipeline_is_listening(void)
{
    pipeline_mode_t m = get_mode();
    return (m == MODE_LISTENING || m == MODE_SPEAKING);
}

void audio_pipeline_deinit(void)
{
    s_running = false;

    /* Wait for tasks to exit */
    if (s_mic_task_hdl)     { while (eTaskGetState(s_mic_task_hdl)     != eDeleted) vTaskDelay(pdMS_TO_TICKS(20)); }
    if (s_detect_task_hdl)  { while (eTaskGetState(s_detect_task_hdl)  != eDeleted) vTaskDelay(pdMS_TO_TICKS(20)); }
    if (s_process_task_hdl) { while (eTaskGetState(s_process_task_hdl) != eDeleted) vTaskDelay(pdMS_TO_TICKS(20)); }
    if (s_playback_task_hdl){ while (eTaskGetState(s_playback_task_hdl)!= eDeleted) vTaskDelay(pdMS_TO_TICKS(20)); }

    /* Free AFE instances */
    if (s_sr_data)  { s_sr_iface->destroy(s_sr_data);   s_sr_data  = NULL; }
    if (s_vc_data)  { s_vc_iface->destroy(s_vc_data);   s_vc_data  = NULL; }
    if (s_models)   { esp_srmodel_deinit(s_models);      s_models   = NULL; }

    /* Free buffers */
    if (s_sr_feed_buf) { free(s_sr_feed_buf); s_sr_feed_buf = NULL; }
    if (s_vc_feed_buf) { free(s_vc_feed_buf); s_vc_feed_buf = NULL; }
    if (s_vc_pcm_buf)  { free(s_vc_pcm_buf);  s_vc_pcm_buf  = NULL; }

    /* Close Opus */
    if (s_opus_enc) { esp_opus_enc_close(s_opus_enc); s_opus_enc = NULL; }
    if (s_opus_dec) { esp_opus_dec_close(s_opus_dec); s_opus_dec = NULL; }

    /* Free queues and mutex */
    if (s_playback_queue) { vQueueDelete(s_playback_queue); s_playback_queue = NULL; }
    if (s_mutex)          { vSemaphoreDelete(s_mutex);      s_mutex = NULL; }
    if (s_playback_stack) { free(s_playback_stack);         s_playback_stack = NULL; }

    ESP_LOGI(TAG, "Audio pipeline deinitialized");
}
