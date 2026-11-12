# Script mode (cmake -P), run by cardenza_target_build_info at every build:
#   -DSOURCE_ROOT=<dir> -DVERSION_FILE=<version.txt> -DOUTPUT=<header> -DNAMESPACE=<ns>
foreach(required SOURCE_ROOT VERSION_FILE OUTPUT NAMESPACE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "generate_build_info.cmake: ${required} is required")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/firmware_info.cmake")
cardenza_read_version("${VERSION_FILE}" BUILD_VERSION)

set(BUILD_HASH "unknown")
set(BUILD_DIRTY false)
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${SOURCE_ROOT}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_ROOT}" rev-parse --short=8 HEAD
                    RESULT_VARIABLE hash_result OUTPUT_VARIABLE BUILD_HASH OUTPUT_STRIP_TRAILING_WHITESPACE)
    # Submodules count: a firmware built on a changed HAL is a firmware with local changes.
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_ROOT}" status --porcelain=v1
                            --untracked-files=normal --ignore-submodules=none
                    RESULT_VARIABLE status_result OUTPUT_VARIABLE git_status OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT hash_result EQUAL 0 OR NOT status_result EQUAL 0)
        message(FATAL_ERROR "generate_build_info.cmake: cannot read Git information of ${SOURCE_ROOT}")
    endif()
    if(NOT git_status STREQUAL "")
        set(BUILD_DIRTY true)
    endif()
endif()

string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S" UTC)
get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/firmware_build_info.h.in" "${OUTPUT}" @ONLY)
