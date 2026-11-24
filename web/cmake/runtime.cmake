# The web target's runtime and the web HAL, for an Emscripten build of a CardenzaHAL firmware.
# The ESP-IDF build shim (web/idf/tools/cmake/project.cmake) uses these; nothing here is part of the
# ESP-IDF build of the HAL.
#
#   cardenza_web_add_runtime(<target>)           the runtime library: kernel (FreeRTOS, pthreads),
#                                                ESP-IDF subset, drivers, entry; PUBLIC include dirs
#   cardenza_web_add_hal(<target> <sdkconfig dir>) the HAL component: CardenzaHAL's own sources over the
#                                                runtime, its keyboard, USB, presence service and system menu replaced
#                                                by web/hal (Net is not built: no network)
#   cardenza_web_link(<executable> <site dir>)   link flags (Asyncify, wraps, exports) and the page

get_filename_component(CARDENZA_WEB_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(CARDENZA_WEB_HAL_DIR "${CARDENZA_WEB_DIR}/.." ABSOLUTE)

# Calls the build wraps so the firmware's allocations land in the emulated RAMs (web/src/esp/heap.cpp)
# and its threads, mutexes and sleeps go through the scheduler (web/src/kernel/pthread.cpp).
set(CARDENZA_WEB_WRAPPED
    malloc free calloc realloc memalign aligned_alloc posix_memalign malloc_usable_size
    pthread_create pthread_join pthread_detach pthread_exit pthread_self sched_yield
    nanosleep clock_nanosleep usleep sleep
    pthread_mutex_init pthread_mutex_destroy pthread_mutex_lock pthread_mutex_trylock pthread_mutex_timedlock pthread_mutex_unlock
    pthread_cond_init pthread_cond_destroy pthread_cond_wait pthread_cond_timedwait pthread_cond_signal pthread_cond_broadcast
    pthread_rwlock_init pthread_rwlock_destroy pthread_rwlock_rdlock pthread_rwlock_tryrdlock pthread_rwlock_wrlock
    pthread_rwlock_trywrlock pthread_rwlock_unlock
    pthread_once pthread_key_create pthread_key_delete pthread_getspecific pthread_setspecific)

function(cardenza_web_add_runtime target)
    file(GLOB runtime_sources CONFIGURE_DEPENDS
        ${CARDENZA_WEB_DIR}/src/*.cpp
        ${CARDENZA_WEB_DIR}/src/kernel/*.cpp
        ${CARDENZA_WEB_DIR}/src/esp/*.cpp
        ${CARDENZA_WEB_DIR}/src/drivers/*.cpp)
    add_library(${target} STATIC ${runtime_sources})
    target_include_directories(${target} PUBLIC ${CARDENZA_WEB_DIR}/include)
    target_include_directories(${target} PRIVATE ${CARDENZA_WEB_HAL_DIR} ${CARDENZA_WEB_DIR}/src)
    target_compile_definitions(${target} PRIVATE CARDENZA_WEB_HAL_SOURCE=1 CARDENZA_WEB_WRAP_MALLOC=1)
    target_compile_features(${target} PUBLIC cxx_std_17)
    target_compile_options(${target} PRIVATE -Wall -Wno-unused-function)
endfunction()

function(cardenza_web_hal_sources out_var)
    set(${out_var}
        ${CARDENZA_WEB_HAL_DIR}/cardenzaHAL.cpp
        ${CARDENZA_WEB_HAL_DIR}/audio/audio.cpp
        ${CARDENZA_WEB_HAL_DIR}/audio/codec/es8156.cpp
        ${CARDENZA_WEB_HAL_DIR}/display/display.cpp
        ${CARDENZA_WEB_HAL_DIR}/ir/ir.cpp
        ${CARDENZA_WEB_HAL_DIR}/mic/mic.cpp
        ${CARDENZA_WEB_HAL_DIR}/storage/app_dir.cpp
        ${CARDENZA_WEB_HAL_DIR}/storage/app_dir_logic.cpp
        ${CARDENZA_WEB_HAL_DIR}/storage/storage.cpp
        ${CARDENZA_WEB_HAL_DIR}/system/settings_logic.cpp
        ${CARDENZA_WEB_HAL_DIR}/system/system.cpp
        ${CARDENZA_WEB_DIR}/hal/camera.cpp
        ${CARDENZA_WEB_DIR}/hal/network.cpp
        ${CARDENZA_WEB_DIR}/hal/keyboard.cpp
        ${CARDENZA_WEB_DIR}/hal/presence.cpp
        ${CARDENZA_WEB_DIR}/hal/settings.cpp
        ${CARDENZA_WEB_DIR}/hal/system_menu.cpp
        ${CARDENZA_WEB_DIR}/hal/usb.cpp
        ${CARDENZA_WEB_DIR}/hal/drive.cpp
        PARENT_SCOPE)
endfunction()

function(cardenza_web_add_hal target runtime)
    cardenza_web_hal_sources(hal_sources)
    add_library(${target} STATIC ${hal_sources})
    target_include_directories(${target} PUBLIC ${CARDENZA_WEB_HAL_DIR})
    target_compile_definitions(${target} PRIVATE CARDENZA_WEB_HAL_SOURCE=1)
    target_compile_features(${target} PUBLIC cxx_std_17)
    target_link_libraries(${target} PUBLIC ${runtime})
endfunction()

# The page around the module: index.html and the script that runs it (web/page).
function(cardenza_web_link executable site_dir)
    set(wraps "")
    foreach(symbol IN LISTS CARDENZA_WEB_WRAPPED)
        list(APPEND wraps "-Wl,--wrap=${symbol}")
    endforeach()
    target_link_options(${executable} PRIVATE
        ${wraps}
        -sASYNCIFY=1
        -sMODULARIZE=1
        -sEXPORT_NAME=createCardenzaFirmware
        -sENVIRONMENT=web,node
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=33554432
        -sSTACK_SIZE=1048576
        -sFORCE_FILESYSTEM=1
        -sEXIT_RUNTIME=0
        -lidbfs.js
        "-sEXPORTED_RUNTIME_METHODS=FS,IDBFS,HEAPU8,HEAPU16,HEAPU32,HEAPF32,UTF8ToString,addRunDependency,removeRunDependency"
    )
    set_target_properties(${executable} PROPERTIES SUFFIX ".js" RUNTIME_OUTPUT_DIRECTORY ${site_dir})
endfunction()
