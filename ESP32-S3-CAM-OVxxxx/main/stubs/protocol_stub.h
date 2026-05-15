#pragma once

#include <string>
#include <functional>
#include <chrono>
#include <vector>
#include <memory>

struct AudioStreamPacket {
    int sample_rate = 0;
    int frame_duration = 0;
    uint32_t timestamp = 0;
    std::vector<uint8_t> payload;
};

enum AbortReason {
    kAbortReasonNone,
    kAbortReasonWakeWordDetected
};

enum ListeningMode {
    kListeningModeAutoStop,
    kListeningModeManualStop,
    kListeningModeRealtime
};

class Protocol {
public:
    virtual ~Protocol() = default;

    inline int server_sample_rate() const { return server_sample_rate_; }
    inline int server_frame_duration() const { return server_frame_duration_; }
    inline const std::string& session_id() const { return session_id_; }

    // Callback registration (AudioService sets these)
    void OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) {
        on_incoming_audio_ = std::move(callback);
    }
    void OnIncomingJson(std::function<void(const std::string& text)> callback) {
        on_incoming_json_text_ = std::move(callback);
    }
    void OnAudioChannelOpened(std::function<void()> callback) {
        on_audio_channel_opened_ = std::move(callback);
    }
    void OnAudioChannelClosed(std::function<void()> callback) {
        on_audio_channel_closed_ = std::move(callback);
    }
    void OnNetworkError(std::function<void(const std::string& message)> callback) {
        on_network_error_ = std::move(callback);
    }
    void OnConnected(std::function<void()> callback) {
        on_connected_ = std::move(callback);
    }
    void OnDisconnected(std::function<void()> callback) {
        on_disconnected_ = std::move(callback);
    }

    virtual bool Start() = 0;
    virtual bool OpenAudioChannel() = 0;
    virtual void CloseAudioChannel(bool send_goodbye = true) = 0;
    virtual bool IsAudioChannelOpened() const = 0;
    virtual bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) = 0;
    virtual void SendWakeWordDetected(const std::string& wake_word) {}
    virtual void SendStartListening(ListeningMode mode) {}
    virtual void SendStopListening() {}
    virtual void SendAbortSpeaking(AbortReason reason) {}

protected:
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> on_incoming_audio_;
    std::function<void(const std::string& text)> on_incoming_json_text_;
    std::function<void()> on_audio_channel_opened_;
    std::function<void()> on_audio_channel_closed_;
    std::function<void(const std::string& message)> on_network_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;

    int server_sample_rate_ = 24000;
    int server_frame_duration_ = 60;
    std::string session_id_;

    virtual bool SendText(const std::string& text) = 0;
};