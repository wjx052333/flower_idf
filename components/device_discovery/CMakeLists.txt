idf_component_register(
    SRCS "device_discovery.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip esp_netif
    PRIV_REQUIRES esp_timer
)
