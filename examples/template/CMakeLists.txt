# A starting point for a Cardenza firmware on CardenzaHAL (CardenzaHAL docs/FIRMWARE-PROJECT.md).
#   idf.py set-target esp32s3 && idf.py build                      # the device: build/my_firmware.bin
#   python3 <HAL>/tools/cardenza_web.py build .                    # the browser: build-web/site
cmake_minimum_required(VERSION 3.16)

# CardenzaHAL: the git submodule components/CardenzaHAL in a firmware of its own (ESP-IDF finds
# components/ by itself), else this repository's root (the template inside CardenzaHAL/examples).
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/components/CardenzaHAL/cardenzaHAL.h")
    set(hal_dir "${CMAKE_CURRENT_LIST_DIR}/components/CardenzaHAL")
else()
    get_filename_component(hal_dir "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    set(EXTRA_COMPONENT_DIRS "${hal_dir}")
endif()

# The board's defaults first, this firmware's after them.
set(SDKCONFIG_DEFAULTS "${hal_dir}/board/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
# The version is version.txt (ESP-IDF reads it into the app description: the version the Launcher shows).
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_firmware)
