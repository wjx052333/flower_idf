set(SRCS
    "main.cc"
    "camera.c"
    "build_info.c"
)

# C++ audio subsystem (from xiaozhi-esp32)
list(APPEND SRCS
    "audio/audio_codec.cc"
    "audio/codecs/box_audio_codec.cc"
    "audio/wake_words/afe_wake_word.cc"
    "audio/wake_words/custom_wake_word.cc"
    "audio/audio_service.cc"
    "audio/demuxer/ogg_demuxer.cc"
    "settings.cc"
)

# Select audio processor according to Kconfig
if(CONFIG_USE_AUDIO_PROCESSOR)
    list(APPEND SRCS "audio/processors/afe_audio_processor.cc")
else()
    list(APPEND SRCS "audio/processors/no_audio_processor.cc")
endif()

if(CONFIG_AUTO_TEST)
    list(APPEND SRCS "auto_test.c" "test_opus_data.c")
endif()

idf_component_register(
    SRCS
        ${SRCS}
        "../../nanopb_gen/proto/flower.pb.c"
        "../../nanopb_gen/proto/mqtt_agent.pb.c"
    INCLUDE_DIRS
        "."
        "../../nanopb_gen/proto"
        "stubs"
        "audio"
        "audio/codecs"
        "audio/wake_words"
        "audio/processors"
        "audio/demuxer"
    REQUIRES
        esp_wifi
        esp_event
        nvs_flash
        esp_mqtt
        mbedtls
        esp_timer
        esp_driver_cam
        esp_driver_i2c
        esp_driver_i2s
        lwip
        nanopb
        esp_http_client
        esp_http_server
        esp_https_ota
        app_update
        esp_new_jpeg
        esp_codec_dev
        esp-sr
        spi_flash
        esp_audio_codec       # provides esp_opus_enc, esp_opus_dec
        esp_audio_effects     # provides esp_ae_rate_cvt
)