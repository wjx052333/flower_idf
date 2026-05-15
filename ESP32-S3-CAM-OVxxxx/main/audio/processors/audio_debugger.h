#pragma once

/* Stub AudioDebugger — no-op implementation. Used when audio debugging is disabled. */

#include <vector>
#include <functional>
#include <cstdint>

class AudioDebugger {
public:
    AudioDebugger() = default;
    ~AudioDebugger() = default;

    void Initialize(int sample_rate, int channels) {}
    void FeedInput(const std::vector<int16_t>& data) {}
    void FeedOutput(const std::vector<int16_t>& data) {}
    void Start() {}
    void Stop() {}
    bool IsRunning() { return false; }
    void OnDebugData(std::function<void(const uint8_t* data, size_t len)> callback) {}
};