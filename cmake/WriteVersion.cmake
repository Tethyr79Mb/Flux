# Lancé à chaque compilation (cmake -P, voir FluxVersion.cmake) : écrit OUTPUT avec FLUX_VERSION et le commit de
# SOURCE_DIR, suivi d'un « + » si des fichiers suivis par Git ont changé depuis. Sans Git : « ? ».
set(commit "")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
            WORKING_DIRECTORY ${SOURCE_DIR}
            OUTPUT_VARIABLE commit
            RESULT_VARIABLE result
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    if(NOT result EQUAL 0)
        set(commit "")
    endif()
    if(commit)
        execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
                WORKING_DIRECTORY ${SOURCE_DIR}
                OUTPUT_VARIABLE changes
                ERROR_QUIET)
        if(changes)
            string(APPEND commit "+")
        endif()
    endif()
endif()
if(NOT commit)
    set(commit "?")
endif()

set(content "// Généré par cmake/WriteVersion.cmake à chaque compilation : ne pas modifier.
#include \"Flux/Core/Version.hpp\"

namespace Flux {
    std::string_view GetVersion() { return \"${FLUX_VERSION}\"; }
    std::string_view GetCommit() { return \"${commit}\"; }
    std::string_view GetFullVersion() { return \"${FLUX_VERSION} (${commit})\"; }
}
")
set(old "")
if(EXISTS ${OUTPUT})
    file(READ ${OUTPUT} old)
endif()
if(NOT old STREQUAL content)
    file(WRITE ${OUTPUT} "${content}")
endif()
