cmake_minimum_required(VERSION 3.20)

# As an ESP-IDF component: a firmware puts this directory in EXTRA_COMPONENT_DIRS and REQUIRES it
# (its directory name, `cardenza` in both firmwares). The HAL then brings its own dependencies,
# TinyUSB among them (idf_component.yml), and configures TinyUSB itself (usb/config/tusb_config.h).
if(COMMAND idf_component_register)
    idf_component_register(
        SRCS
            cardenzaHAL.cpp
            audio/audio.cpp
            audio/codec/es8156.cpp
            display/display.cpp
            ir/ir.cpp
            keyboard/keyboard.cpp
            keyboard/internal/keyboard_hardware.cpp
            keyboard/internal/keyboard_key_scan.cpp
            keyboard/internal/keyboard_led_scan.cpp
            mic/mic.cpp
            storage/storage.cpp
            system/system.cpp
            usb/usb.cpp
        INCLUDE_DIRS
            .
        REQUIRES
            driver
            esp_lcd
            esp_timer
            fatfs
            freertos
            sdmmc
            vfs
            espressif__tinyusb
        PRIV_REQUIRES
            esp_hw_support
            esp_rom
            log
            usb
    )
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
    # Two TinyUSB calls are wrapped for the audio feedback endpoint (usb/usb.cpp): its TX FIFO size and the
    # format of its value on full speed.
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=usbd_edpt_iso_alloc" "-Wl,--wrap=usbd_edpt_xfer")
    idf_build_get_property(build_components BUILD_COMPONENTS)
    if(tinyusb IN_LIST build_components)
        set(cardenza_tinyusb tinyusb)
    else()
        set(cardenza_tinyusb espressif__tinyusb)
    endif()
    idf_component_get_property(cardenza_tinyusb_lib ${cardenza_tinyusb} COMPONENT_LIB)
    target_include_directories(${cardenza_tinyusb_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/usb/config")
    return()
endif()

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
    ${CMAKE_CURRENT_LIST_DIR}/usb/usb.cpp
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
