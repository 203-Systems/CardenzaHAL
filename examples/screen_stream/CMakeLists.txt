cmake_minimum_required(VERSION 3.16)

# The screen stream's test firmware (usb/camera.h): it turns the camera on and draws scenes that cost the stream
# differently. Built in place, with the HAL two levels up.
get_filename_component(hal_dir "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(EXTRA_COMPONENT_DIRS "${hal_dir}")
set(SDKCONFIG_DEFAULTS "${hal_dir}/board/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(screen_stream)
