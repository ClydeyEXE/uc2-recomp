# Writes OUT (a header defining UCC2_VERSION) on every build, touching it only when the version changes.
# From a git checkout the version is `git describe`, with "-modified" when tracked files have changes; from a
# GitHub source zip, which has no history, it is tools/version.txt, which git fills in as it makes the zip
# (export-subst in .gitattributes).
execute_process(COMMAND git describe --tags --always
                WORKING_DIRECTORY "${SRC}" OUTPUT_VARIABLE version RESULT_VARIABLE failed
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(failed OR version STREQUAL "")
    file(STRINGS "${SRC}/tools/version.txt" version LIMIT_COUNT 1)
    if(version MATCHES "^\\$Format" OR version STREQUAL "")
        set(version "unknown")
    endif()
else()
    # The toolkit always shows as changed (its patches are applied in place), so it is left out.
    execute_process(COMMAND git status --porcelain --untracked-files=no --ignore-submodules=all
                    WORKING_DIRECTORY "${SRC}" OUTPUT_VARIABLE changes ERROR_QUIET)
    if(NOT changes STREQUAL "")
        string(APPEND version "-modified")
    endif()
endif()

set(text "#define UCC2_VERSION \"${version}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
