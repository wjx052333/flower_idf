#pragma once

/* Minimal Board singleton stub — provides GetAudioCodec() for AudioService. */

class AudioCodec;

class Board {
public:
    static Board& GetInstance() {
        static Board instance;
        return instance;
    }

    AudioCodec* GetAudioCodec() { return audio_codec_; }
    void SetAudioCodec(AudioCodec* codec) { audio_codec_ = codec; }

private:
    Board() = default;
    AudioCodec* audio_codec_ = nullptr;
};