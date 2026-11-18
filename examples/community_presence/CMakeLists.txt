# The community presence service (net/presence.h) in a firmware: CardenzaHAL::Init starts it (Kconfig
# CONFIG_CARDENZA_HAL_PRESENCE, on by default), the firmware only says what to do before a handover.
#   idf.py -C <HAL>/examples/community_presence build
cmake_minimum_required(VERSION 3.16)
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/../../board/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/../launcher_net/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
get_filename_component(cardenza_hal_dir "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(EXTRA_COMPONENT_DIRS "${cardenza_hal_dir}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(community_presence)
