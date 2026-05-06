cmake_minimum_required(VERSION 3.16)

# Reuse the nanopb component already set up in the relay project.
# Run flower_idf/relay/setup_proto.sh once first if not already done.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../relay/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cam_mqtt)
