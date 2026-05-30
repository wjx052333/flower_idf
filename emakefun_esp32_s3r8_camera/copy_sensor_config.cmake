# copy_sensor_config.cmake
# Called at build time by the restore_sensor_config target.
# Reads SENSOR_PROFILE env var and copies the matching profile_*.h file
# into managed_components as the active OV3660 JPEG config.
#
# Usage: cmake -DSRC=<dir> -DDST=<dir> -P copy_sensor_config.cmake
#        SENSOR_PROFILE=a_bright|b_balanced|c_clean|d_detail

if(NOT SRC OR NOT DST)
    message(FATAL_ERROR "SRC and DST must be defined")
endif()

# set(PROFILE "$ENV{SENSOR_PROFILE}")
# set(PROFILE "origin")
# set(PROFILE "b_balanced")
# set(PROFILE "c_clean")
set(PROFILE "d_detail")
if(PROFILE STREQUAL "")
    set(PROFILE "a_bright")
endif()

set(SRC_FILE "${SRC}/profile_${PROFILE}.h")
set(DST_FILE "${DST}/ov3660_dvp_8bit_10Minput_1280x720_jpeg_12fps.h")

if(NOT EXISTS "${SRC_FILE}")
    message(FATAL_ERROR "Profile file not found: ${SRC_FILE}")
endif()

file(MAKE_DIRECTORY "${DST}")
file(COPY "${SRC_FILE}" DESTINATION "${DST}")
file(RENAME "${DST}/profile_${PROFILE}.h" "${DST_FILE}")

message(STATUS "Sensor config: profile_${PROFILE}.h → ${DST_FILE}")