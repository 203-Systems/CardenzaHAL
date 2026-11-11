# cmake/firmware_info.cmake without a firmware: a version is read and checked, the header is generated
# with the namespace asked for, and a dirty Git tree is seen. Run with cmake -DWORK=<scratch dir> -P.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/firmware_info.cmake")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/src")
file(WRITE "${WORK}/src/version.txt" "1.2.3\n")
cardenza_read_version("${WORK}/src/version.txt" version)
if(NOT version STREQUAL "1.2.3")
    message(FATAL_ERROR "version: ${version}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -DSOURCE_ROOT=${WORK}/src -DVERSION_FILE=${WORK}/src/version.txt
                        -DOUTPUT=${WORK}/out/Info.h -DNAMESPACE=demo::build
                        -P ${CMAKE_CURRENT_LIST_DIR}/../cmake/generate_build_info.cmake RESULT_VARIABLE result)
file(READ "${WORK}/out/Info.h" header)
if(NOT result EQUAL 0 OR NOT header MATCHES "namespace demo::build" OR NOT header MATCHES "kVersion\\[\\] = \"1.2.3\""
   OR NOT header MATCHES "kHash\\[\\] = \"unknown\"")
    message(FATAL_ERROR "header (no Git):\n${header}")
endif()
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${WORK}/src -c init.defaultBranch=main init -q)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${WORK}/src -c user.name=t -c user.email=t@t add -A)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${WORK}/src -c user.name=t -c user.email=t@t commit -q -m init)
    file(WRITE "${WORK}/src/edit.txt" "x")
    execute_process(COMMAND ${CMAKE_COMMAND} -DSOURCE_ROOT=${WORK}/src -DVERSION_FILE=${WORK}/src/version.txt
                            -DOUTPUT=${WORK}/out/Info.h -DNAMESPACE=demo::build
                            -P ${CMAKE_CURRENT_LIST_DIR}/../cmake/generate_build_info.cmake)
    file(READ "${WORK}/out/Info.h" header)
    if(header MATCHES "kHash\\[\\] = \"unknown\"" OR NOT header MATCHES "kDirty = true")
        message(FATAL_ERROR "header (Git, edited):\n${header}")
    endif()
endif()
file(WRITE "${WORK}/src/version.txt" "1.2\n")
execute_process(COMMAND ${CMAKE_COMMAND} -DSOURCE_ROOT=${WORK}/src -DVERSION_FILE=${WORK}/src/version.txt
                        -DOUTPUT=${WORK}/out/Bad.h -DNAMESPACE=demo::build
                        -P ${CMAKE_CURRENT_LIST_DIR}/../cmake/generate_build_info.cmake RESULT_VARIABLE result
                        OUTPUT_QUIET ERROR_QUIET)
if(result EQUAL 0)
    message(FATAL_ERROR "a version that is not major.minor.patch was taken")
endif()
message(STATUS "build info ok")
