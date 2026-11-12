# PSRAM in the heap only for what asks for it, and `new` that never reboots the board: an example
# firmware for Cardenza (ESP32-S3R2, 2 MB PSRAM). Build it like any ESP-IDF project:
#   idf.py -C <HAL>/examples/psram_heap build
cmake_minimum_required(VERSION 3.16)
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/../../board/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(psram_heap)
