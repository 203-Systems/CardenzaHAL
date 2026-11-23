# mic-check's pure code on the computer (no ESP-IDF): the level math against synthetic signals, the results'
# checksum and log lines, and every screen drawn into PNGs.
#   cmake -S examples/mic-check/test -B <build> && cmake --build <build> && ctest --test-dir <build>
#   <build>/mic_check_shots <folder>          # the screens as PNGs
cmake_minimum_required(VERSION 3.16)
project(mic_check_test CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(main ${CMAKE_CURRENT_LIST_DIR}/../main)
add_library(mic_check_pure STATIC ${main}/levels.cpp ${main}/results.cpp ${main}/screens.cpp ${main}/font.cpp)
target_include_directories(mic_check_pure PUBLIC ${main})
target_compile_options(mic_check_pure PRIVATE -Wall -Wextra -Werror)

add_executable(mic_check_levels_test levels_test.cpp)
target_link_libraries(mic_check_levels_test mic_check_pure)
add_executable(mic_check_shots shots.cpp)
target_link_libraries(mic_check_shots mic_check_pure)

enable_testing()
add_test(NAME levels COMMAND mic_check_levels_test)
add_test(NAME screens COMMAND mic_check_shots ${CMAKE_CURRENT_BINARY_DIR}/shots)
