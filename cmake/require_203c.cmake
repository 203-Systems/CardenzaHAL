# A firmware on CardenzaHAL is made, tried on a Cardenza and published with 203c, the 203 community's CLI
# (docs/FIRMWARE-PROJECT.md: it is required). The HAL checks for it when a firmware's build is configured, so
# every way of building stops on a computer without it (idf.py build, tools/cardenza.py build, the web target)
# and says how to install it. 203c is found as tools/cardenza.py finds it: on PATH, else the Agent Skill's copy
# run by node. CI builds skip the check: their publish step installs 203c itself.
# Then it prints 203c's update notices (`203c updates --quiet`: a newer 203c, this firmware's CardenzaHAL submodule
# behind its main or not a clean copy) as a warning: from 203c's answer of the day, nothing when all is current or
# offline, and only from a 203c that has the command. It came after 3.5.0, so 3.5.1, the release after it, is the
# first that can (an older 203c is not asked; one that refuses is not heard).
set(CARDENZA_HAL_203C_WANTED 3.5.0)
set(CARDENZA_HAL_203C_UPDATES 3.5.1)

function(cardenza_hal_require_203c)
    if(CMAKE_BUILD_EARLY_EXPANSION OR DEFINED ENV{CI} OR DEFINED ENV{GITHUB_ACTIONS})
        return()
    endif()
    set(install "npm install -g 203c && 203c skill install")
    set(rules "https://cardenza.203.io/community/develop.md")

    # On Windows npm installs 203c.cmd beside a shell script named 203c: the .cmd is the one that runs there.
    find_program(CARDENZA_HAL_203C NAMES 203c.cmd 203c)
    set(command "${CARDENZA_HAL_203C}")
    if(NOT CARDENZA_HAL_203C)
        set(command "")
        if(DEFINED ENV{CLAUDE_SKILLS_DIR})
            set(skills "$ENV{CLAUDE_SKILLS_DIR}")
        elseif(DEFINED ENV{HOME})
            set(skills "$ENV{HOME}/.claude/skills")
        else()
            set(skills "$ENV{USERPROFILE}/.claude/skills")
        endif()
        set(script "${skills}/203-community/scripts/203c.mjs")
        find_program(CARDENZA_HAL_NODE NAMES node)
        if(CARDENZA_HAL_NODE AND EXISTS "${script}")
            set(command "${CARDENZA_HAL_NODE}" "${script}")
        endif()
    endif()

    if(NOT command)
        message(FATAL_ERROR
            "CardenzaHAL: 203c is not installed on this computer, and a Cardenza firmware does not build without it. "
            "203c, the 203 community's CLI, is required for every Cardenza firmware: it starts the project (203c new), "
            "installs a build on the Cardenza through its 203 Launcher (203c flash; never idf.py flash, which erases "
            "the Launcher), reads its log (203c log) and publishes it (203c publish). "
            "Install it (Node.js 18 or later; macOS, Linux and Windows alike), then build again:\n"
            "   ${install}\n"
            "Cloning the 203-CLI repository does not install it. The rules for every step: ${rules}")
    endif()

    if(WIN32 AND command MATCHES "\\.[cC][mM][dD]$")
        set(command cmd /c "${command}")
    endif()
    execute_process(COMMAND ${command} --version
        OUTPUT_VARIABLE output RESULT_VARIABLE result ERROR_VARIABLE error
        OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 30)
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" version "${output}")
    if(NOT result EQUAL 0 OR NOT version)
        message(FATAL_ERROR
            "CardenzaHAL: 203c is installed (${command}) but does not run: ${error} "
            "Install it again (it needs Node.js 18 or later), then build again:\n"
            "   ${install}")
    endif()
    if(version VERSION_LESS CARDENZA_HAL_203C_WANTED)
        message(WARNING "CardenzaHAL: 203c ${version} is older than this HAL expects (${CARDENZA_HAL_203C_WANTED}); update it: ${install}")
    elseif(NOT version VERSION_LESS CARDENZA_HAL_203C_UPDATES)
        if(DEFINED PROJECT_DIR)
            set(project "${PROJECT_DIR}")
        else()
            set(project "${CMAKE_SOURCE_DIR}")
        endif()
        execute_process(COMMAND ${command} updates --quiet --project "${project}"
            OUTPUT_VARIABLE notices RESULT_VARIABLE result ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 5)
        if(result EQUAL 0 AND NOT notices STREQUAL "")
            string(REPLACE "\n" "\n   " notices "${notices}")
            message(WARNING "CardenzaHAL: updates (from 203c updates; NO_UPDATE_NOTIFIER=1 turns this off):\n   ${notices}")
        endif()
    endif()
endfunction()
