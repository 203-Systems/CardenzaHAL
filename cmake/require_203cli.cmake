# A firmware on CardenzaHAL is made, tried on a Cardenza and published with 203cli, the 203 community's CLI
# (docs/FIRMWARE-PROJECT.md: it is required). The HAL checks for it when a firmware's build is configured, so
# every way of building stops on a computer without it (idf.py build, tools/cardenza.py build, the web target)
# and says how to install it. 203cli is found as tools/cardenza.py finds it: on PATH, else the Agent Skill's copy
# run by node. CI builds skip the check: their publish step installs it itself.
# 203cli was called 203c (renamed 2026-11-20): where only 203c is installed (on PATH, or an older skill's
# scripts/203c.mjs) the build goes on with a warning that says to install 203cli.
# Then it prints 203cli's update notices (`203cli updates --quiet`: a newer 203cli, this firmware's CardenzaHAL submodule
# behind its main or not a clean copy) as a warning: from the answer of the day, nothing when all is current or
# offline. 203cli has the command from its first version, 3.5.1; so has the last 203c (3.5.1, which runs 203cli and
# adds that 203c is renamed); an older 203c is not asked, one that refuses is not heard.
set(CARDENZA_HAL_203CLI_WANTED 3.5.1)
set(CARDENZA_HAL_203CLI_UPDATES 3.5.1)

function(cardenza_hal_require_203cli)
    if(CMAKE_BUILD_EARLY_EXPANSION OR DEFINED ENV{CI} OR DEFINED ENV{GITHUB_ACTIONS})
        return()
    endif()
    set(install "npm install -g 203cli && 203cli skill install")
    set(rules "https://cardenza.203.io/community/develop.md")
    if(DEFINED ENV{CLAUDE_SKILLS_DIR})
        set(skills "$ENV{CLAUDE_SKILLS_DIR}")
    elseif(DEFINED ENV{HOME})
        set(skills "$ENV{HOME}/.claude/skills")
    else()
        set(skills "$ENV{USERPROFILE}/.claude/skills")
    endif()
    find_program(CARDENZA_HAL_NODE NAMES node)

    # 203cli; else the old name, 203c. On Windows npm installs <name>.cmd beside a shell script named <name>: the
    # .cmd is the one that runs there.
    set(command "")
    set(name "")
    foreach(candidate 203cli 203c)
        string(TOUPPER "${candidate}" upper)
        find_program(CARDENZA_HAL_${upper} NAMES ${candidate}.cmd ${candidate})
        if(CARDENZA_HAL_${upper})
            set(command "${CARDENZA_HAL_${upper}}")
        elseif(CARDENZA_HAL_NODE AND EXISTS "${skills}/203-community/scripts/${candidate}.mjs")
            set(command "${CARDENZA_HAL_NODE}" "${skills}/203-community/scripts/${candidate}.mjs")
        endif()
        if(command)
            set(name ${candidate})
            break()
        endif()
    endforeach()

    if(NOT command)
        message(FATAL_ERROR
            "CardenzaHAL: 203cli is not installed on this computer, and a Cardenza firmware does not build without it. "
            "203cli, the 203 community's CLI, is required for every Cardenza firmware: it starts the project (203cli new), "
            "installs a build on the Cardenza through its 203 Launcher (203cli flash; never idf.py flash, which erases "
            "the Launcher), reads its log (203cli log) and publishes it (203cli publish). "
            "Install it (Node.js 18 or later; macOS, Linux and Windows alike), then build again:\n"
            "   ${install}\n"
            "Cloning the 203-CLI repository does not install it. The rules for every step: ${rules}")
    endif()
    if(name STREQUAL "203c")
        message(WARNING "CardenzaHAL: 203c is renamed 203cli, the only CLI of the 203 community now; 203c gets no more "
            "updates or support. This build goes on with 203c (${command}). Install 203cli (then npm uninstall -g 203c):\n"
            "   ${install}")
    endif()

    set(run ${command})
    if(WIN32 AND command MATCHES "\\.[cC][mM][dD]$")
        set(run cmd /c "${command}")
    endif()
    execute_process(COMMAND ${run} --version
        OUTPUT_VARIABLE output RESULT_VARIABLE result ERROR_VARIABLE error
        OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 30)
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" version "${output}")
    if(NOT result EQUAL 0 OR NOT version)
        message(FATAL_ERROR
            "CardenzaHAL: ${name} is installed (${command}) but does not run: ${error} "
            "Install 203cli (it needs Node.js 18 or later), then build again:\n"
            "   ${install}")
    endif()
    if(version VERSION_LESS CARDENZA_HAL_203CLI_WANTED)
        # (An older 203c was told above: installing 203cli is the update.)
        if(name STREQUAL "203cli")
            message(WARNING "CardenzaHAL: 203cli ${version} is older than this HAL expects (${CARDENZA_HAL_203CLI_WANTED}); update it: ${install}")
        endif()
    elseif(NOT version VERSION_LESS CARDENZA_HAL_203CLI_UPDATES)
        if(DEFINED PROJECT_DIR)
            set(project "${PROJECT_DIR}")
        else()
            set(project "${CMAKE_SOURCE_DIR}")
        endif()
        execute_process(COMMAND ${run} updates --quiet --project "${project}"
            OUTPUT_VARIABLE notices RESULT_VARIABLE result ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 5)
        # (That 203c is renamed was said above.)
        string(REGEX REPLACE "[^\n]*203c is renamed 203cli[^\n]*\n?" "" notices "${notices}")
        string(STRIP "${notices}" notices)
        if(result EQUAL 0 AND NOT notices STREQUAL "")
            string(REPLACE "\n" "\n   " notices "${notices}")
            message(WARNING "CardenzaHAL: updates (from ${name} updates; NO_UPDATE_NOTIFIER=1 turns this off):\n   ${notices}")
        endif()
    endif()
endfunction()
