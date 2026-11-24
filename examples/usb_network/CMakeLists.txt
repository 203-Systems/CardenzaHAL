# The board as a USB network adapter (usb/network.h): a page at http://cardenza.local (and http://192.168.203.1)
# with the board's information and the link's state; a key switches to the uplink mode (the computer's shared
# internet) and fetches a page through it. Build it like any ESP-IDF project:
#   idf.py -C <HAL>/examples/usb_network build
# (No web build: it uses ESP-IDF's esp_http_server and esp_http_client, which the web target does not have.)
cmake_minimum_required(VERSION 3.16)
get_filename_component(hal_dir "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# The HAL is this repository's root; main needs no REQUIRES (ESP-IDF's main depends on every component).
set(EXTRA_COMPONENT_DIRS "${hal_dir}")
set(SDKCONFIG_DEFAULTS "${hal_dir}/board/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(usb_network)
