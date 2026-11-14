# ESP-IDF's project.cmake for the CardenzaHAL web target.
#
# A firmware's top-level CMakeLists.txt does what it does for the device,
#   include($ENV{IDF_PATH}/tools/cmake/project.cmake)
#   project(my_firmware)
# and with IDF_PATH pointing here (tools/cardenza_web.py build sets it) and Emscripten's toolchain
# (emcmake), the same project builds into a page: index.html, <project>.js, <project>.wasm. Nothing of
# ESP-IDF is needed. This file is the ESP-IDF build system in short:
#
# - project() finds the components as ESP-IDF does: <project>/main, <project>/components/*,
#   EXTRA_COMPONENT_DIRS and COMPONENT_DIRS (a folder with a CMakeLists.txt is a component, else its
#   subfolders are), <project>/managed_components/* (from a device build) except TinyUSB, which the
#   web target replaces. A component is named after its folder; a project component wins over one of
#   the same name found later.
# - Each component's CMakeLists.txt runs (add_subdirectory) and calls idf_component_register, which
#   makes the static library __idf_<name>; REQUIRES and PRIV_REQUIRES are linked once every component
#   is known. Without REQUIRES, main requires every component, as in ESP-IDF.
# - ESP-IDF's own components (freertos, driver, esp_lcd, ...) are the web target's runtime: every
#   component gets its headers (web/include) and the project's sdkconfig.h.
# - The component holding cardenzaHAL.h is CardenzaHAL: its folder is never built. The web HAL is
#   built in its place, from the HAL this file belongs to (web/cmake/runtime.cmake).
# - sdkconfig.h comes from the project's sdkconfig when it has one (a device build's full
#   configuration), else from SDKCONFIG_DEFAULTS over ESP-IDF's defaults (tools/cardenza_web.py
#   sdkconfig); every CONFIG_ is a CMake variable too.
# - The page (web/page) and sim.json are put next to the module after it links
#   (tools/cardenza_web.py assemble).
#
# What a device build does that this does not: Kconfig (menuconfig), linker fragments, partition
# tables and images, the component manager (managed_components is taken as it is), prebuilt libraries
# and assembly sources (Xtensa code cannot run here: a firmware that has them does not build).

include_guard(GLOBAL)

get_filename_component(CARDENZA_WEB_IDF_PATH "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(IDF_PATH "${CARDENZA_WEB_IDF_PATH}")
set(IDF_TARGET esp32s3)
set(IDF_VER "v5.3.5-cardenza-web")
# A firmware's CMake can tell the web target by this (and its code by the CARDENZA_HAL_WEB definition),
# where it has to: a vendored library with a web build of its own that keys on __EMSCRIPTEN__, say.
set(CARDENZA_HAL_WEB TRUE)
include("${CARDENZA_WEB_IDF_PATH}/../cmake/runtime.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/component.cmake")

# ESP-IDF's project(): the builtin project(), then the whole build.
macro(project project_name)
    # Asyncify unwinds and rewinds the call stack: it needs the optimizer. A firmware's own build
    # type wins when it names one.
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    _project(${project_name} C CXX)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "cardenza-web: configure with Emscripten's toolchain (emcmake cmake ..., or tools/cardenza_web.py build)")
    endif()
    __cardenza_web_project("${project_name}")
endmacro()

macro(__cardenza_web_project project_name)
    set(PROJECT_DIR "${CMAKE_SOURCE_DIR}")
    set(BUILD_DIR "${CMAKE_BINARY_DIR}")
    idf_build_set_property(PROJECT_NAME "${project_name}")
    idf_build_set_property(PROJECT_DIR "${PROJECT_DIR}")
    idf_build_set_property(BUILD_DIR "${BUILD_DIR}")
    __cardenza_web_project_version(PROJECT_VER)
    idf_build_set_property(PROJECT_VER "${PROJECT_VER}")

    # C as ESP-IDF 5.3 compiles it (gnu17, gnu++2b); its optimization for size and speed is -O2 here.
    set(CMAKE_C_STANDARD 17)
    set(CMAKE_C_EXTENSIONS ON)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_EXTENSIONS ON)
    set(CMAKE_C_FLAGS_RELEASE "-O2")
    set(CMAKE_CXX_FLAGS_RELEASE "-O2")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-O2")
    set(CMAKE_C_FLAGS_DEBUG "-O1 -g")
    set(CMAKE_CXX_FLAGS_DEBUG "-O1 -g")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-O1 -g -sASSERTIONS=2 -sSTACK_OVERFLOW_CHECK=2")

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    __cardenza_web_sdkconfig()

    # Where the page goes (tools/cardenza_web.py build --out).
    set(CARDENZA_WEB_SITE_DIR "${CMAKE_BINARY_DIR}/site" CACHE PATH "Output folder of the web build (the page)")

    __cardenza_web_make_idf_interface()
    cardenza_web_add_runtime(cardenza_web_runtime)
    set_target_properties(cardenza_web_runtime PROPERTIES CXX_STANDARD 17)
    target_link_libraries(cardenza_web_runtime PUBLIC cardenza_web_idf)
    __cardenza_web_identity("${project_name}" "${PROJECT_VER}")
    target_compile_definitions(cardenza_web_runtime PRIVATE
        "CARDENZA_WEB_PROJECT_NAME=\"${project_name}\""
        "CARDENZA_WEB_PROJECT_VER=\"${PROJECT_VER}\""
        "CARDENZA_WEB_BUILD_HASH=\"${CARDENZA_WEB_BUILD_HASH}\""
        "CARDENZA_HAL_VERSION=\"${CARDENZA_WEB_HAL_VERSION}\"")
    # The web HAL is always there: the runtime's page interface reaches its keyboard and USB.
    cardenza_web_hal_sources(cardenza_web_hal_sources)
    add_library(cardenza_web_hal STATIC ${cardenza_web_hal_sources})
    target_include_directories(cardenza_web_hal PUBLIC "${CARDENZA_WEB_HAL_DIR}")
    target_compile_definitions(cardenza_web_hal PRIVATE CARDENZA_WEB_HAL_SOURCE=1)
    target_link_libraries(cardenza_web_hal PUBLIC cardenza_web_runtime cardenza_web_idf)

    __cardenza_web_find_components()
    __cardenza_web_add_components()
    __cardenza_web_link_requirements()
    __cardenza_web_scrub_device_flags()
    __cardenza_web_executable("${project_name}")
    __cardenza_web_write_project_json("${project_name}" "${PROJECT_VER}")
endmacro()
