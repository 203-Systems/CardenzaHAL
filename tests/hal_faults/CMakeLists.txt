# hal_root: the HAL's root, set by ../CMakeLists.txt.
set(fake_include ${CMAKE_CURRENT_BINARY_DIR}/sdk-headers)
# Generate include-name shims in the build tree, keeping test SDK definitions in one file.
foreach(header esp_log.h esp_attr.h esp_check.h esp_ipc.h esp_timer.h esp_system.h esp_vfs_fat.h sdmmc_cmd.h
    esp_lcd_panel_io.h esp_lcd_panel_ops.h esp_lcd_panel_commands.h esp_lcd_panel_vendor.h
    driver/gpio.h driver/i2s_std.h driver/i2s_pdm.h driver/i2c_master.h driver/spi_master.h
    driver/gptimer.h driver/ledc.h driver/sdmmc_host.h driver/rmt_encoder.h driver/rmt_tx.h
    freertos/FreeRTOS.h freertos/task.h freertos/semphr.h freertos/queue.h
    soc/gpio_struct.h soc/spi_periph.h hal/spi_ll.h)
  get_filename_component(parent ${fake_include}/${header} DIRECTORY)
  file(MAKE_DIRECTORY ${parent})
  file(WRITE ${fake_include}/${header} "#pragma once\n#include \"esp_stub.h\"\n")
endforeach()
add_library(CardenzaHalFaultSources STATIC
  hardware_boundary.cpp
  ${hal_root}/cardenzaHAL.cpp
  ${hal_root}/audio/audio.cpp ${hal_root}/audio/codec/es8156.cpp
  ${hal_root}/display/display.cpp ${hal_root}/storage/storage.cpp
  ${hal_root}/ir/ir.cpp ${hal_root}/mic/mic.cpp
  ${hal_root}/keyboard/keyboard.cpp
  ${hal_root}/keyboard/internal/keyboard_key_scan.cpp
  ${hal_root}/keyboard/internal/keyboard_led_scan.cpp
)
target_include_directories(CardenzaHalFaultSources PUBLIC ${fake_include} ${CMAKE_CURRENT_SOURCE_DIR}/fakes ${hal_root})
# The HAL uses designated initializers, which GCC takes in C++17 and MSVC only in C++20.
target_compile_features(CardenzaHalFaultSources PUBLIC cxx_std_20)
add_executable(CardenzaHalFaultTests hal_fault_test.cpp)
target_link_libraries(CardenzaHalFaultTests PRIVATE CardenzaHalFaultSources)
add_executable(CardenzaDisplayPresentTests display_present_test.cpp)
target_link_libraries(CardenzaDisplayPresentTests PRIVATE CardenzaHalFaultSources)
foreach(fault mutex task pwm gpio timer_new timer_callback timer_alarm timer_enable timer_start)
  add_test(NAME HalKeyboard_${fault} COMMAND CardenzaHalFaultTests keyboard ${fault})
endforeach()
foreach(fault panel_reset panel_init spi_bus display_pwm)
  add_test(NAME HalDisplay_${fault} COMMAND CardenzaHalFaultTests display ${fault})
endforeach()
foreach(scenario cold warm dirty timeout warm_timeout draw brightness display_duty_update display_callback display_semaphore native invalid)
  add_test(NAME HalDisplayPresent_${scenario} COMMAND CardenzaDisplayPresentTests ${scenario})
endforeach()
foreach(fault i2c state)
  add_test(NAME HalCodec_${fault} COMMAND CardenzaHalFaultTests codec ${fault})
endforeach()
foreach(fault normal unmount)
  add_test(NAME HalStorage_${fault} COMMAND CardenzaHalFaultTests storage ${fault})
endforeach()
add_test(NAME HalInfraredNec COMMAND CardenzaHalFaultTests ir normal)
add_test(NAME HalCodecInitOwnership COMMAND CardenzaHalFaultTests codec ownership)
add_test(NAME HalCodecInitStopFailure COMMAND CardenzaHalFaultTests codec stop)
# MSVC: a failed assert ends the test instead of waiting on a dialog (../WindowsTestGuard.cpp).
if(MSVC)
  foreach(target CardenzaHalFaultTests CardenzaDisplayPresentTests)
    target_sources(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../WindowsTestGuard.cpp)
  endforeach()
endif()
