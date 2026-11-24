# The SD card as a USB drive (usb/drive.h): a key lends the card to the computer and takes it back; the screen
# shows the state. Build it like any ESP-IDF project:
#   idf.py -C <HAL>/examples/usb_drive build
cmake_minimum_required(VERSION 3.16)
get_filename_component(hal_dir "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# The HAL is this repository's root; main needs no REQUIRES (ESP-IDF's main depends on every component).
set(EXTRA_COMPONENT_DIRS "${hal_dir}")
set(SDKCONFIG_DEFAULTS "${hal_dir}/board/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(usb_drive)
