idf_component_register(
    SRCS
        "main.c"
        "camera.c"
        "audio.c"
        "audio_pipeline.c"
        "build_info.c"
        "auto_test.c"
        "test_opus_data.c"
        "proto/flower.pb.c"
        "proto/mqtt_agent.pb.c"
    INCLUDE_DIRS
        "."
        "proto"
    REQUIRES
        esp_wifi
        esp_event
        nvs_flash
        esp_mqtt
        mbedtls
        esp_timer
        esp_driver_cam          # provides esp_cam_ctlr.h (CAM_CTLR_DATA_WIDTH_8)
        esp_driver_i2c          # provides driver/i2c_master.h for pre-init with pullups
        esp_driver_i2s          # provides driver/i2s_tdm.h / driver/i2s_std.h
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
)
