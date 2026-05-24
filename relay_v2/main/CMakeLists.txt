idf_component_register(
    SRCS
        "main.c"
        "ds18b20.c"
        "../../nanopb_gen/proto/flower.pb.c"
    INCLUDE_DIRS
        "."
        "../../nanopb_gen/proto"
    REQUIRES
        esp_wifi
        esp_event
        nvs_flash
        esp_mqtt
        mbedtls
        esp_timer
        esp_http_server
        driver
        esp_driver_gpio
        esp_driver_ledc
        esp_adc
        esp_driver_uart
        lwip
        nanopb
        esp_https_ota
        app_update
)

target_link_libraries(${COMPONENT_LIB} m)
