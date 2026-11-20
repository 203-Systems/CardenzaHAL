# Host tests of the HAL's own code: no board and no ESP-IDF needed.
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
# - logic_test, key_count_guard_test: the pure calculations the HAL runs on the device (key debounce and
#   the key-count guard, LED frames, the backlight curve).
# - net_test: the pure parts of net/ (the Launcher's settings format, request checks, connect order).
# - presence_test: the presence service's decisions (net/presence_logic.h) and the Launcher's restart ticket.
# - system_menu_test: the system menu's pure parts (system/menu/logic.h).
# - hal_faults: the HAL's driver sources against fake ESP-IDF drivers (fakes/esp_stub.h) that fail on
#   purpose, so every error path is taken and leaves the hardware in a known state.
cmake_minimum_required(VERSION 3.20)
project(CardenzaHALTests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(hal_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
enable_testing()

# MSVC: a failed assert ends the test instead of waiting on a dialog.
set(cardenza_test_guard "")
if(MSVC)
  set(cardenza_test_guard ${CMAKE_CURRENT_SOURCE_DIR}/WindowsTestGuard.cpp)
endif()

add_executable(CardenzaHalLogicTests logic_test.cpp ${cardenza_test_guard})
target_include_directories(CardenzaHalLogicTests PRIVATE ${hal_root})
add_test(NAME CardenzaHalLogicTests COMMAND CardenzaHalLogicTests)

add_executable(CardenzaKeyCountGuardTests key_count_guard_test.cpp ${cardenza_test_guard})
target_include_directories(CardenzaKeyCountGuardTests PRIVATE ${hal_root})
add_test(NAME CardenzaKeyCountGuardTests COMMAND CardenzaKeyCountGuardTests)

# keyboard/key_settle.h and keyboard/led_light.h: pure helpers above the keyboard driver.
add_executable(CardenzaKeyboardHelperTests keyboard_helpers_test.cpp ${cardenza_test_guard})
target_include_directories(CardenzaKeyboardHelperTests PRIVATE ${hal_root})
add_test(NAME CardenzaKeyboardHelperTests COMMAND CardenzaKeyboardHelperTests)

# display/changed_regions.h: what PresentChanged sends.
add_executable(CardenzaChangedRegionsTests changed_regions_test.cpp ${cardenza_test_guard})
target_include_directories(CardenzaChangedRegionsTests PRIVATE ${hal_root})
add_test(NAME CardenzaChangedRegionsTests COMMAND CardenzaChangedRegionsTests)

# net/: the Launcher's saved-network storage (CRC-32 keys, base64, AES-128-CBC against vectors made by
# the Launcher's own code), request checks, the order networks are tried in, the NVS page check.
add_executable(CardenzaNetTests net_test.cpp
  ${hal_root}/net/connect_order.cpp ${hal_root}/net/http_request.cpp ${hal_root}/net/launcher_settings.cpp
  ${cardenza_test_guard})
target_include_directories(CardenzaNetTests PRIVATE ${hal_root})
add_test(NAME CardenzaNetTests COMMAND CardenzaNetTests)

# net/presence_logic.h (the presence service's decisions: allowed hosts, the inbox answer, the handover
# plan, backoff, giving way to audio and RAM) and system/launcher.h (the Launcher's restart ticket).
add_executable(CardenzaPresenceTests presence_test.cpp
  ${hal_root}/net/presence_logic.cpp ${hal_root}/net/http_request.cpp ${hal_root}/net/launcher_settings.cpp
  ${cardenza_test_guard})
target_include_directories(CardenzaPresenceTests PRIVATE ${hal_root})
add_test(NAME CardenzaPresenceTests COMMAND CardenzaPresenceTests)

# system/menu/logic.h and frame.h: the system menu's press, curtain, keys, list, words, its frames (the
# Launcher's, still and sliding, tests/system_menu_frames.inc) and what a slide's frames send to the panel
# (display/changed_regions.h), NVS reader.
add_executable(CardenzaSystemMenuTests system_menu_test.cpp ${hal_root}/system/menu/logic.cpp
  ${hal_root}/system/menu/frame.cpp ${hal_root}/system/menu/launcher/glyphs.cpp
  ${hal_root}/system/menu/launcher/lcd.cpp ${cardenza_test_guard})
target_include_directories(CardenzaSystemMenuTests PRIVATE ${hal_root})
add_test(NAME CardenzaSystemMenuTests COMMAND CardenzaSystemMenuTests)

add_subdirectory(hal_faults)

# cmake/firmware_info.cmake: version.txt, the generated header, Git's commit and local changes.
add_test(NAME CardenzaBuildInfoTests COMMAND ${CMAKE_COMMAND} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/build-info-test
         -P ${CMAKE_CURRENT_SOURCE_DIR}/build_info_test.cmake)

# tools/cardenza_flash.py: partition tables, the Launcher on a board, build tables that would overwrite it.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME CardenzaFlashToolTests COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/flash_tool_test.py)
  # tools/cardenza_usb.py (SysEx framing, limits) and tools/check_submodule.py (a scratch git repository).
  add_test(NAME CardenzaHostToolsTests COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/host_tools_test.py)
  # tools/cardenza_web.py (the web target's build tool): sdkconfig, build failures, the HAL, sim.json; with
  # Emscripten installed it also builds examples/psram_heap for the web.
  add_test(NAME CardenzaWebToolTests COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/web_tool_test.py)
  # tools/cardenza.py (the preferred build tool): the project, ESP-IDF, 203c and the checks a build stops at.
  add_test(NAME CardenzaDevToolTests COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/dev_tool_test.py)
  # cmake/require_203c.cmake: every firmware's build stops without 203c and says how to install it.
  add_test(NAME CardenzaRequire203cTests COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/require_203c_test.py)
endif()

# The web target's pure parts (web/): the kernel, the allocator, the display model, the audio rings.
add_subdirectory(web)
