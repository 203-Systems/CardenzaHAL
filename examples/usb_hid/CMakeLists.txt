# USB HID inputs (usb/hid.h): the Cardenza as the computer's keyboard, media keys and gamepad.
#   idf.py -C <HAL>/examples/usb_hid build
cmake_minimum_required(VERSION 3.16)
get_filename_component(hal_dir "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# The HAL is this repository's root; main needs no REQUIRES (ESP-IDF's main depends on every component).
set(EXTRA_COMPONENT_DIRS "${hal_dir}")
set(SDKCONFIG_DEFAULTS "${hal_dir}/board/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(usb_hid)
