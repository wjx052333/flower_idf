cmake_minimum_required(VERSION 3.16)

# nanopb from relay project; esp_mqtt from OV5640 sibling project
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../relay/components")
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../ESP32-S3-CAM-OVxxxx/components")

set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.private")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.private")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(emakefun_cam_mqtt)

add_custom_target(touch_build_info ALL
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_SOURCE_DIR}/main/build_info.c
    COMMENT "Refreshing build timestamp"
)

# ── Sensor config profile injection ──────────────────────────────────────
# managed_components is gitignored and wiped by fullclean. Modified OV3660
# register tables live in main/sensor_config/ under git and are copied into
# managed_components at build time (before esp_cam_sensor compiles).
#
# Select profile via SENSOR_PROFILE env var:
#   SENSOR_PROFILE=a_bright   idf.py build    (default, brightness-first)
#   SENSOR_PROFILE=b_balanced idf.py build    (balanced)
#   SENSOR_PROFILE=c_clean    idf.py build    (clean-first)
#   SENSOR_PROFILE=x_original idf.py build    (original, before parametric tuning)

set(SENSOR_CFG_SRC "${CMAKE_CURRENT_SOURCE_DIR}/main/sensor_config")
set(SENSOR_CFG_DST "${CMAKE_CURRENT_SOURCE_DIR}/managed_components/espressif__esp_cam_sensor/sensors/ov3660/private_include")

add_custom_target(restore_sensor_config ALL
    COMMAND ${CMAKE_COMMAND}
        "-DSRC=${SENSOR_CFG_SRC}"
        "-DDST=${SENSOR_CFG_DST}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/copy_sensor_config.cmake"
    COMMENT "Restoring sensor config profile (SENSOR_PROFILE=$ENV{SENSOR_PROFILE})"
)

add_dependencies(__idf_espressif__esp_cam_sensor restore_sensor_config)
