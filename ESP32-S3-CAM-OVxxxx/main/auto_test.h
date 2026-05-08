#pragma once
#include "esp_err.h"
#include "mqtt_client.h"

/**
 * Start automatic MQTT voice test task.
 *
 * After MQTT connects, runs:
 *   Round 1 — tools inquiry ("你有哪些工具可以用")
 *   Pause 5s
 *   Round 2 — 3-turn conversation
 *
 * Uses pre-encoded Opus data from test_opus_data.h. Publishes AudioFrame
 * protobuf messages to @p topic_up_opus and AgentRequest to @p topic_up_agent.
 *
 * @param client          MQTT client handle
 * @param mqtt_connected  Pointer to the connected-flag (polled by task)
 * @param topic_up_opus   Topic for uplink AudioFrame publish
 * @param topic_up_agent  Topic for AgentRequest publish
 */
esp_err_t auto_test_start(esp_mqtt_client_handle_t client,
                          const volatile bool *mqtt_connected,
                          const char *topic_up_opus,
                          const char *topic_up_agent);
