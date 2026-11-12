# Net (net/net.h) in a firmware: the 203 Launcher's Wi-Fi and account read without writing anything,
# one HTTPS request, and everything given back. Build it like any ESP-IDF project:
#   idf.py -C <HAL>/examples/launcher_net build
# sdkconfig.defaults is the reference set of options a firmware using Net needs.
cmake_minimum_required(VERSION 3.16)
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/../../board/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
# The HAL as a firmware takes it: its directory is the component (named after the directory).
get_filename_component(cardenza_hal_dir "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(EXTRA_COMPONENT_DIRS "${cardenza_hal_dir}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(launcher_net)
