# wisp_version(<out_var>): the firmware version string, shared by the
# device build (firmware/CMakeLists.txt, as PROJECT_VER, so it lands in
# esp_app_desc_t) and the simulator (simulator/CMakeLists.txt).
#
# A checkout at or after a release tag vX.Y.Z gives "X.Y.Z[-N-gHASH]";
# without such a tag "<WISP_BASE_VERSION>-<commits>-g<hash>". Either way a
# "-dirty" suffix marks uncommitted changes to tracked files. The shape is
# what core::compare_firmware_versions() orders, so the device can tell an
# upgrade from a downgrade (connectivity spec 6.5); it fits the 31
# characters esp_app_desc_t::version holds. Without git: the base version.
#
# Evaluated at configure time. The configure step re-runs after a commit
# or checkout (it depends on .git/logs/HEAD), but not after merely editing
# a file, so "-dirty" can lag until the next reconfigure.

set(WISP_BASE_VERSION "0.3.1")

function(wisp_version out_var)
    set(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    set(version "${WISP_BASE_VERSION}")
    find_package(Git QUIET)
    if(GIT_FOUND AND EXISTS "${root}/.git")
        if(EXISTS "${root}/.git/logs/HEAD")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${root}/.git/logs/HEAD")
        endif()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*.[0-9]*.[0-9]*" --dirty
            WORKING_DIRECTORY "${root}"
            OUTPUT_VARIABLE described
            RESULT_VARIABLE described_rc
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(described_rc EQUAL 0 AND described MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+")
            string(REGEX REPLACE "^v" "" version "${described}")
        else()
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD
                WORKING_DIRECTORY "${root}"
                OUTPUT_VARIABLE commits
                RESULT_VARIABLE commits_rc
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
                WORKING_DIRECTORY "${root}"
                OUTPUT_VARIABLE hash
                RESULT_VARIABLE hash_rc
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
                WORKING_DIRECTORY "${root}"
                OUTPUT_VARIABLE changes
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(commits_rc EQUAL 0 AND hash_rc EQUAL 0)
                set(version "${WISP_BASE_VERSION}-${commits}-g${hash}")
                if(NOT changes STREQUAL "")
                    string(APPEND version "-dirty")
                endif()
            endif()
        endif()
    endif()
    set(${out_var} "${version}" PARENT_SCOPE)
endfunction()
