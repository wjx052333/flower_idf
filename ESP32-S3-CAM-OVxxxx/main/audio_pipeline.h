#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * audio_pipeline.h — Unified audio subsystem (xiaozhi-esp32 dual-AFE pattern)
 *
 * Architecture:
 *   IDLE:       AFE(SR mode) → WakeNet continuous wake word detection
 *   LISTENING:  AFE(VC mode) → AEC+NS+BSS → VAD → clean mono → Opus encode → callback
 *   SPEAKING:   AFE(VC mode) stays running (echo cancellation), downlink Opus → decode → speaker
 *
 * Two independent AFE instances, never active simultaneously. Mode switching
 * hands off mic ownership cleanly between them.
 */

typedef struct {
    /** Wake word detected (from IDLE mode). Typically starts a voice session. */
    void (*on_wake_word)(void *user_data);

    /** VAD state changed (during LISTENING mode). speaking=true when voice detected. */
    void (*on_vad_change)(bool speaking, void *user_data);

    /** Encoded Opus packet ready for MQTT publish (during LISTENING mode). */
    void (*on_uplink_opus)(const uint8_t *data, size_t len, uint64_t seq,
                           uint64_t timestamp_ms, bool is_eos, void *user_data);

    /** Downlink TTS stream finished (EOS received from server). May be NULL. */
    void (*on_downlink_eos)(void *user_data);

    void *user_data;
} audio_pipeline_callbacks_t;

/**
 * Initialize the audio pipeline. Must be called after audio_hw_init().
 * Creates both AFE instances, Opus encoder/decoder, and background tasks.
 */
esp_err_t audio_pipeline_init(const audio_pipeline_callbacks_t *callbacks);

/** Transition from IDLE → LISTENING. AFE(VC) activates, wake word pauses. */
esp_err_t audio_pipeline_start_listening(void);

/** Transition from LISTENING/SPEAKING → IDLE. AFE(SR) resumes wake word detection. */
esp_err_t audio_pipeline_stop_listening(void);

/** Feed a downlink Opus packet for decoding and speaker playback. */
esp_err_t audio_pipeline_feed_downlink(const uint8_t *opus_data, size_t len, bool is_eos);

/** Returns true if currently in LISTENING or SPEAKING mode. */
bool audio_pipeline_is_listening(void);

/** Deinitialize pipeline, free all resources, stop all tasks. */
void audio_pipeline_deinit(void);
