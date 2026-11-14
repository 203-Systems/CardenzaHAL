# Host tests of the web target's pure parts (web/): the kernel (FreeRTOS on ucontexts, a simulated
# clock), the RAM regions' allocator, the ST7789 model, the audio rings and the web keyboard. The
# browser side is tested by tests/web/browser (Playwright) and tools/cardenza_web.py's tests.
set(web_dir ${hal_root}/web)

add_executable(CardenzaWebAllocatorTests allocator_test.cpp ${web_dir}/src/esp/region_allocator.cpp)
target_include_directories(CardenzaWebAllocatorTests PRIVATE ${web_dir}/src/esp)
add_test(NAME CardenzaWebAllocatorTests COMMAND CardenzaWebAllocatorTests)

add_executable(CardenzaWebDisplayTests st7789_test.cpp ${web_dir}/src/drivers/st7789.cpp)
target_include_directories(CardenzaWebDisplayTests PRIVATE ${web_dir}/src/drivers)
add_test(NAME CardenzaWebDisplayTests COMMAND CardenzaWebDisplayTests)

add_executable(CardenzaWebAudioTests audio_rings_test.cpp ${web_dir}/src/drivers/audio_rings.cpp)
target_include_directories(CardenzaWebAudioTests PRIVATE ${web_dir}/src/drivers)
add_test(NAME CardenzaWebAudioTests COMMAND CardenzaWebAudioTests)

if(NOT WIN32)
    # The kernel switches contexts with ucontext on the host (Emscripten fibers in the browser).
    set(kernel_sources
        ${web_dir}/src/kernel/context.cpp
        ${web_dir}/src/kernel/kernel.cpp
        ${web_dir}/src/kernel/raw_alloc.cpp
        ${web_dir}/src/kernel/freertos_task.cpp
        ${web_dir}/src/kernel/freertos_queue.cpp
        ${web_dir}/src/kernel/freertos_misc.cpp
        ${web_dir}/src/esp/esp_timer.cpp
        host_stubs.cpp)
    add_executable(CardenzaWebKernelTests kernel_test.cpp ${kernel_sources})
    add_executable(CardenzaWebKeyboardTests keyboard_test.cpp ${web_dir}/hal/keyboard.cpp ${kernel_sources})
    foreach(test CardenzaWebKernelTests CardenzaWebKeyboardTests)
        target_include_directories(${test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${web_dir}/include ${hal_root})
        target_compile_definitions(${test} PRIVATE CARDENZA_WEB_HAL_SOURCE=1)
        add_test(NAME ${test} COMMAND ${test})
    endforeach()
endif()
