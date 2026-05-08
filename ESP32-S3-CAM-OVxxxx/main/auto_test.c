/**
 * auto_test.c — Automatic MQTT voice test
 *
 * Streams pre-encoded Opus frames to the agent, simulating two test scenarios:
 *   1. Tools inquiry
 *   2. 3-turn conversation (with 5s pause between rounds)
 *
 * No Opus encoder needed — data is pre-encoded and stored in test_opus_data.c.
 * Format: [u16_le frame_len][opus_payload]...
 */

#include "auto_test.h"
#include "test_opus_data.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "pb_encode.h"
#include "proto/mqtt_agent.pb.h"

#define TAG "AutoTest"

/* ── Nanopb helpers ──────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *data;
    size_t         len;
} bytes_buf_t;

static bool encode_bytes_callback(pb_ostream_t *stream, const pb_field_t *field,
                                   void *const *arg)
{
    const bytes_buf_t *buf = (const bytes_buf_t *)(*arg);
    if (!pb_encode_tag_for_field(stream, field)) return false;
    return pb_encode_string(stream, buf->data, buf->len);
}

static bool noop_encode(pb_ostream_t *stream, const pb_field_t *field, void *const *arg)
{
    (void)stream; (void)field; (void)arg;
    return true;
}

/* ── Static state ─────────────────────────────────────────────────────────── */

static esp_mqtt_client_handle_t s_client         = NULL;
static const char              *s_topic_up_opus   = NULL;
static const char              *s_topic_up_agent  = NULL;
static const volatile bool     *s_mqtt_connected = NULL;
static SemaphoreHandle_t        s_eos_sem         = NULL;

/* ── Send helpers ─────────────────────────────────────────────────────────── */

static void publish_agent_request(mqtt_agent_agent_action_t action)
{
    mqtt_agent_agent_request_t req = MQTT_AGENT_AGENT_REQUEST_INIT_ZERO;
    req.action = action;
    req.chat_id.funcs.encode = noop_encode;

    uint8_t buf[32];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, &mqtt_agent_agent_request_t_msg, &req)) return;
    esp_mqtt_client_publish(s_client, s_topic_up_agent,
                            (const char *)buf, (int)stream.bytes_written, 1, 0);
}

static void publish_opus_frame(const uint8_t *frame, size_t len,
                                uint64_t seq, bool is_eos)
{
    mqtt_agent_audio_frame_t af = MQTT_AGENT_AUDIO_FRAME_INIT_ZERO;
    af.seq          = seq;
    af.timestamp_ms = esp_timer_get_time() / 1000;
    af.is_eos       = is_eos;

    if (!is_eos && frame && len > 0) {
        bytes_buf_t opus_buf = { .data = frame, .len = len };
        af.opus_data.funcs.encode = encode_bytes_callback;
        af.opus_data.arg = &opus_buf;
    }

    uint8_t buf[512];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, &mqtt_agent_audio_frame_t_msg, &af)) return;
    esp_mqtt_client_publish(s_client, s_topic_up_opus,
                            (const char *)buf, (int)stream.bytes_written, 1, 0);
}

/* ── Stream pre-encoded Opus data ─────────────────────────────────────────── */

/**
 * Walk through packed Opus data and publish each frame.
 * Returns the next sequence number (after EOS).
 */
static uint64_t stream_opus_packed(const uint8_t *data, size_t size,
                                    uint64_t start_seq)
{
    size_t   pos = 0;
    uint64_t seq = start_seq;

    while (pos + 2 <= size) {
        size_t frame_len = (size_t)data[pos] | ((size_t)data[pos + 1] << 8);
        pos += 2;
        if (frame_len == 0 || pos + frame_len > size) break;

        publish_opus_frame(&data[pos], frame_len, seq++, false);
        pos += frame_len;

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* EOS */
    publish_opus_frame(NULL, 0, seq++, true);
    return seq;
}

/* ── Test task ────────────────────────────────────────────────────────────── */

static void test_task(void *arg)
{
    /* Wait for MQTT */
    ESP_LOGI(TAG, "Waiting for MQTT connection...");
    while (!*s_mqtt_connected)
        vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "MQTT connected — starting auto-test sequence");

    /* Clean stale agent state */
    publish_agent_request(MQTT_AGENT_AGENT_ACTION_AGENT_ACTION_STOP);
    vTaskDelay(pdMS_TO_TICKS(5500));

    /* Start agent session */
    publish_agent_request(MQTT_AGENT_AGENT_ACTION_AGENT_ACTION_CHAT);
    vTaskDelay(pdMS_TO_TICKS(500));

    /* ════ Round 1: Tools inquiry ════ */
    ESP_LOGI(TAG, "══════ Round 1: Tools Inquiry ══════");
    ESP_LOGI(TAG, "Sending: '你有哪些工具可以用' (%d bytes packed Opus)",
             (int)test_opus_tools_size);
    stream_opus_packed(test_opus_tools, test_opus_tools_size, 0);
    ESP_LOGI(TAG, "Round 1 done");

    /* Wait for Round 1 TTS EOS — server takes ~11s for LLM+TTS, cap at 30s */
    ESP_LOGI(TAG, "Waiting for Round 1 TTS EOS...");
    xSemaphoreTake(s_eos_sem, pdMS_TO_TICKS(30000));
    vTaskDelay(pdMS_TO_TICKS(2000));  /* brief pause after playback ends */

    /* ════ Round 2: Multi-turn conversation ════ */
    ESP_LOGI(TAG, "══════ Round 2: Multi-turn Conversation ══════");

    /* Re-establish agent session — Round 1 session terminated after 10s idle */
    publish_agent_request(MQTT_AGENT_AGENT_ACTION_AGENT_ACTION_CHAT);
    vTaskDelay(pdMS_TO_TICKS(500));

    const struct {
        const uint8_t *data;
        size_t         size;
        const char    *text;
    } turns[] = {
        { test_opus_multiturn_0, test_opus_multiturn_0_size, "你好，我是测试设备五号" },
        { test_opus_multiturn_1, test_opus_multiturn_1_size, "今天天气怎么样" },
        { test_opus_multiturn_2, test_opus_multiturn_2_size, "帮我开灯" },
    };

    uint64_t global_seq = 0;
    for (int i = 0; i < 3; i++) {
        ESP_LOGI(TAG, "--- Turn %d: '%s' (%d bytes) ---",
                 i + 1, turns[i].text, (int)turns[i].size);
        global_seq = stream_opus_packed(turns[i].data, turns[i].size, global_seq);
        ESP_LOGI(TAG, "Turn %d done, waiting for TTS EOS...", i + 1);
        xSemaphoreTake(s_eos_sem, pdMS_TO_TICKS(30000));
        vTaskDelay(pdMS_TO_TICKS(1000));  /* brief pause between turns */
    }
    ESP_LOGI(TAG, "Round 2 done");

    /* Cleanup */
    publish_agent_request(MQTT_AGENT_AGENT_ACTION_AGENT_ACTION_STOP);
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "══════ Auto-test completed ══════");
    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void auto_test_on_downlink_eos(void *user_data)
{
    (void)user_data;
    if (s_eos_sem)
        xSemaphoreGive(s_eos_sem);
}

esp_err_t auto_test_start(esp_mqtt_client_handle_t client,
                          const volatile bool *mqtt_connected,
                          const char *topic_up_opus,
                          const char *topic_up_agent)
{
    if (!client || !mqtt_connected || !topic_up_opus || !topic_up_agent)
        return ESP_ERR_INVALID_ARG;

    s_eos_sem = xSemaphoreCreateBinary();
    if (!s_eos_sem) return ESP_ERR_NO_MEM;

    s_client         = client;
    s_mqtt_connected = mqtt_connected;
    s_topic_up_opus  = topic_up_opus;
    s_topic_up_agent = topic_up_agent;

    if (xTaskCreate(test_task, "auto_test", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create auto_test task");
        vSemaphoreDelete(s_eos_sem);
        s_eos_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Auto-test task created");
    return ESP_OK;
}
