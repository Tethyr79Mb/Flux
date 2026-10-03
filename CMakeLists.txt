cmake_minimum_required(VERSION 3.28)
project(Flux LANGUAGES C CXX) # C : requis par volk

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Toutes les DLL à côté des exe : Windows ne les trouve pas ailleurs.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Symboles cachés par défaut partout : seul ce qui porte FLUX_API est exporté.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT WIN32)
    message(FATAL_ERROR "Flux ne cible que Windows pour l'instant (Platform/Win32).")
endif()

if(MSVC)
    # Sources en UTF-8 (accents des chaînes et des commentaires) : sans ça, MSVC les lit dans la page de code de Windows.
    # /Zc:__cplusplus : __cplusplus donne la vraie version du standard (MSVC reste sinon à 199711L).
    add_compile_options(/utf-8 /Zc:__cplusplus)
endif()

if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    # Ninja apprend de quels en-têtes dépend chaque .cpp en lisant les lignes « inclusion du fichier » de /showIncludes.
    # Un MSVC en français les écrit avec des espaces insécables, dans un encodage qui change avec /utf-8 : Ninja ne les
    # reconnaissait pas et ne recompilait rien quand un .hpp changeait (des .obj périmés faisaient planter l'éditeur).
    # MSVC parle donc anglais pendant la compilation (VSLANG=1033), avec le préfixe anglais, quelle que soit la langue
    # de Visual Studio.
    set(CMAKE_C_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env VSLANG=1033)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env VSLANG=1033)
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file:")

    # Vérifie que MSVC parle bien anglais : sinon, prévenir plutôt que de garder en silence des .obj périmés.
    set(showdir ${CMAKE_BINARY_DIR}/CMakeFiles/FluxShowIncludes)
    file(WRITE ${showdir}/a.h "\n")
    file(WRITE ${showdir}/a.c "#include \"a.h\"\n")
    execute_process(
        COMMAND ${CMAKE_C_COMPILER_LAUNCHER} ${CMAKE_C_COMPILER} /nologo /utf-8 /showIncludes /c a.c
        WORKING_DIRECTORY ${showdir} OUTPUT_VARIABLE showOutput ERROR_VARIABLE showOutput)
    if(NOT showOutput MATCHES "Note: including file: *[^\n]*a\\.h")
        message(WARNING "MSVC n'affiche pas « Note: including file: » : Ninja ne suivra pas les en-têtes.\n${showOutput}")
    endif()
endif()

include(cmake/FluxWarnings.cmake)
include(cmake/FluxDependencies.cmake)
include(cmake/FluxResources.cmake)
include(cmake/FluxSources.cmake)
include(cmake/FluxPrecompiled.cmake)

add_subdirectory(Engine)
add_subdirectory(Game)
add_subdirectory(Editor)

enable_testing()
add_subdirectory(Tests)
