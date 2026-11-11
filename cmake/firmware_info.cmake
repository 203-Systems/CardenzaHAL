# A firmware's version and build information, for any firmware on the HAL. Plain CMake: usable in the
# ESP-IDF project file and in host builds (simulators, tests) alike.
#
#   include(<HAL>/cmake/firmware_info.cmake)
#   cardenza_read_version(${CMAKE_CURRENT_LIST_DIR}/version.txt PROJECT_VER)   # before project():
#                       # the app description's version, which the 203 Launcher shows
#   cardenza_target_build_info(<target> <source root> [HEADER FirmwareBuildInfo.h] [NAMESPACE firmware::build])
#
# The header holds kVersion (version.txt), kHash (the source root's Git commit, 8 digits, or "unknown"),
# kDirty (built with local changes) and kTimeUtc. It is generated at every build, so a commit or an
# edit shows without reconfiguring. Print them first thing at every start, with the ELF's hash
# (esp_app_get_elf_sha256), and keep the ELF of every build you give anyone: that is how a crash's
# backtrace is read later.
include_guard(GLOBAL)

set(CARDENZA_FIRMWARE_INFO_DIR "${CMAKE_CURRENT_LIST_DIR}")

# version.txt holds major.minor.patch; anything else fails the configure.
function(cardenza_read_version version_file out_var)
    file(READ "${version_file}" version)
    string(STRIP "${version}" version)
    if(NOT version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "${version_file} must hold a major.minor.patch version, not '${version}'")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${version_file}")
    set(${out_var} "${version}" PARENT_SCOPE)
endfunction()

function(cardenza_target_build_info target source_root)
    cmake_parse_arguments(INFO "" "HEADER;NAMESPACE;VERSION_FILE" "" ${ARGN})
    if(NOT INFO_HEADER)
        set(INFO_HEADER FirmwareBuildInfo.h)
    endif()
    if(NOT INFO_NAMESPACE)
        set(INFO_NAMESPACE firmware::build)
    endif()
    if(NOT INFO_VERSION_FILE)
        set(INFO_VERSION_FILE "${source_root}/version.txt")
    endif()
    set(info_dir "${CMAKE_CURRENT_BINARY_DIR}/build-info")
    set(info_header "${info_dir}/${INFO_HEADER}")
    # Runs for every build, including incremental builds after a commit or a local edit.
    add_custom_target(${target}BuildInfo
        COMMAND ${CMAKE_COMMAND}
            "-DSOURCE_ROOT=${source_root}"
            "-DVERSION_FILE=${INFO_VERSION_FILE}"
            "-DOUTPUT=${info_header}"
            "-DNAMESPACE=${INFO_NAMESPACE}"
            -P "${CARDENZA_FIRMWARE_INFO_DIR}/generate_build_info.cmake"
        BYPRODUCTS "${info_header}"
        VERBATIM
    )
    add_dependencies(${target} ${target}BuildInfo)
    target_sources(${target} PRIVATE "${info_header}")
    target_include_directories(${target} PRIVATE "${info_dir}")
endfunction()
