cmake_minimum_required(VERSION 3.20)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(CardenzaHAL LANGUAGES C CXX)
endif()

set(cardenza_hal_sources
    ${CMAKE_CURRENT_LIST_DIR}/cardenzaHAL.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/audio.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/codec/es8156.cpp
    ${CMAKE_CURRENT_LIST_DIR}/display/display.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ir/ir.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mic/mic.cpp
    ${CMAKE_CURRENT_LIST_DIR}/system/system.cpp
)

set(cardenza_hal_embedded_default OFF)
if(DEFINED IDF_TARGET OR DEFINED COMPONENT_DIR)
    set(cardenza_hal_embedded_default ON)
endif()

option(CARDENZA_HAL_ENABLE_EMBEDDED_DRIVERS "Build Cardenza HAL sources that require ESP-IDF drivers" ${cardenza_hal_embedded_default})

if(NOT CARDENZA_HAL_ENABLE_EMBEDDED_DRIVERS)
    message(FATAL_ERROR "CardenzaHAL is ESP-IDF only. Use DEVICE=CardenzaSim for desktop host builds.")
endif()

list(APPEND cardenza_hal_sources
    ${CMAKE_CURRENT_LIST_DIR}/keyboard/keyboard.cpp
    ${CMAKE_CURRENT_LIST_DIR}/keyboard/internal/keyboard_hardware.cpp
    ${CMAKE_CURRENT_LIST_DIR}/keyboard/internal/keyboard_key_scan.cpp
    ${CMAKE_CURRENT_LIST_DIR}/keyboard/internal/keyboard_led_scan.cpp
    ${CMAKE_CURRENT_LIST_DIR}/storage/storage.cpp
)

add_library(CardenzaHAL)

target_sources(CardenzaHAL PRIVATE
    ${cardenza_hal_sources}
)

target_include_directories(CardenzaHAL PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_features(CardenzaHAL PUBLIC cxx_std_17)
