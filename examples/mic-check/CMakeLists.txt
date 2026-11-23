# mic-check: the board's microphone, raw, at several distances and loudnesses (main/main.cpp), to choose the
# default microphone gain from measurements. A test firmware: it is not published.
#   idf.py -C <HAL>/examples/mic-check build                  # build/mic-check.bin, version.txt's version
#   cmake -S <HAL>/examples/mic-check/test -B <dir>            # its pure code on the computer (test/)
# Its own defaults after the board's: the presence service off (no Wi-Fi beside the microphone), a larger
# main task (the UI prints the table), the HAL's partition table (the image goes to ota_0 at 0x170000).
cmake_minimum_required(VERSION 3.16)
get_filename_component(hal_dir "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(EXTRA_COMPONENT_DIRS "${hal_dir}")
set(SDKCONFIG_DEFAULTS "${hal_dir}/board/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mic-check)
