idf_component_register(
    SRCS
        "main.c"
        "proto/device.pb.c"
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
        esp_http_server
        driver
        lwip
        nanopb
)

target_link_libraries(${COMPONENT_LIB} m)
