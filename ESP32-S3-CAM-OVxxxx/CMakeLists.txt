cmake_minimum_required(VERSION 3.16)

# Reuse the nanopb component already set up in the relay project.
# Run flower_idf/relay/setup_proto.sh once first if not already done.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../relay/components")

# Include private sdkconfig defaults if present (gitignored — copy from sdkconfig.defaults
# and fill in your WiFi + MQTT credentials)
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.private")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.private")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cam_mqtt)

# Touch build_info.c on every build to force fresh __DATE__/__TIME__
add_custom_target(touch_build_info ALL
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_SOURCE_DIR}/main/build_info.c
    COMMENT "Refreshing build timestamp in build_info.c"
)

# SR models are auto-packed by esp-sr's movemodel.py during build.
# CONFIG_SR_WN_WN9_HIESP=y and CONFIG_SR_NSN_NSNET2=y in sdkconfig.defaults
# tell movemodel.py which models to include in srmodels/srmodels.bin.
