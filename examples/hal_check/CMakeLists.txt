# The board through the HAL: the screen, every key and its LED, HOME and the speaker. The same source
# builds for the device and for the browser:
#   idf.py -C <HAL>/examples/hal_check build
#   python3 <HAL>/tools/cardenza_web.py build <HAL>/examples/hal_check
cmake_minimum_required(VERSION 3.16)
get_filename_component(hal_dir "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# The HAL is this repository's root; main needs no REQUIRES (ESP-IDF's main depends on every component).
set(EXTRA_COMPONENT_DIRS "${hal_dir}")
set(SDKCONFIG_DEFAULTS "${hal_dir}/board/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hal_check)
