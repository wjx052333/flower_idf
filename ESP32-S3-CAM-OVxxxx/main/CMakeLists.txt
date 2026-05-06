idf_component_register(
    SRCS
        "main.c"
        "camera.c"
        "proto/flower.pb.c"
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
        lwip
        nanopb
        esp_http_client
        esp_http_server
        esp_https_ota
        app_update
        esp_new_jpeg
)
