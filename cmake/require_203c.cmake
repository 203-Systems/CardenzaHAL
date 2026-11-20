# The old name of require_203cli.cmake (203c is renamed 203cli): a build that includes this file by its old name gets
# the same check, and cardenza_hal_require_203c() is cardenza_hal_require_203cli().
include(${CMAKE_CURRENT_LIST_DIR}/require_203cli.cmake)

function(cardenza_hal_require_203c)
    cardenza_hal_require_203cli()
endfunction()
