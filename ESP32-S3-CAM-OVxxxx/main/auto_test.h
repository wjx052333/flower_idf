#pragma once
#include "sdkconfig.h"
#include "esp_err.h"
#include "mqtt_client.h"

/**
 * Start automatic MQTT voice test task.
 *
 * After MQTT connects, runs:
 *   Round 1 — tools inquiry ("你有哪些工具可以用")
 *   Round 2 — 3-turn conversation (waits for downlink TTS EOS between turns)
 *
 * Uses pre-encoded Opus data from test_opus_data.h. Publishes AudioFrame
 * protobuf messages to @p topic_up_opus and AgentRequest to @p topic_up_agent.
 *
 * Register auto_test_on_downlink_eos as the on_downlink_eos callback in
 * audio_pipeline_callbacks_t so the test task knows when TTS finishes.
 *
 * @param client          MQTT client handle
 * @param mqtt_connected  Pointer to the connected-flag (polled by task)
 * @param topic_up_opus   Topic for uplink AudioFrame publish
 * @param topic_up_agent  Topic for AgentRequest publish
 */
#ifdef CONFIG_AUTO_TEST
esp_err_t auto_test_start(esp_mqtt_client_handle_t client,
                          const volatile bool *mqtt_connected,
                          const char *topic_up_opus,
                          const char *topic_up_agent);

/** Register as audio_pipeline_callbacks_t.on_downlink_eos */
void auto_test_on_downlink_eos(void *user_data);
#else
static inline esp_err_t auto_test_start(esp_mqtt_client_handle_t c,
    const volatile bool *m, const char *o, const char *a)
{ (void)c; (void)m; (void)o; (void)a; return ESP_OK; }
static inline void auto_test_on_downlink_eos(void *u) { (void)u; }
#endif
