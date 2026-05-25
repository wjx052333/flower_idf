cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../relay/components")
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../ESP32-S3-CAM-OVxxxx/components")

set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.private")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.private")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ai_thinker_cam_mqtt)

add_custom_target(touch_build_info ALL
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_SOURCE_DIR}/main/build_info.c
    COMMENT "Refreshing build timestamp"
)
