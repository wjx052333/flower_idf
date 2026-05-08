cmake_minimum_required(VERSION 3.16)

# Reuse the nanopb component already set up in the relay project.
# Run flower_idf/relay/setup_proto.sh once first if not already done.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../relay/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cam_mqtt)

# Touch build_info.c on every build to force fresh __DATE__/__TIME__
add_custom_target(touch_build_info ALL
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_SOURCE_DIR}/main/build_info.c
    COMMENT "Refreshing build timestamp in build_info.c"
)

# IDF's partition system generates an empty srmodels.bin during cmake configure
# (inside project() above). We must overwrite it POST_BUILD so it sticks.
set(SRMODELS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/srmodels.bin")
if(EXISTS "${SRMODELS_SRC}")
    add_custom_command(TARGET cam_mqtt.elf POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SRMODELS_SRC}"
            "${CMAKE_BINARY_DIR}/srmodels/srmodels.bin"
        COMMENT "Overwriting empty srmodels.bin with real model data (629 KB)"
    )
else()
    message(WARNING "srmodels.bin not found in project root — run scripts/pack_models.py first")
endif()
