# Host tests of the HAL's own code: no board and no ESP-IDF needed.
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
# - logic_test, key_count_guard_test: the pure calculations the HAL runs on the device (key debounce and
#   the key-count guard, LED frames, the backlight curve).
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

add_subdirectory(hal_faults)
