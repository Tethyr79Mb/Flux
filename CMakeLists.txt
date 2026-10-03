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
    # Avec /utf-8, un MSVC en français écrit ses « Remarque : inclusion du fichier : » (espaces insécables) autrement
    # que lors de la détection par CMake : Ninja ne reconnaît plus ces lignes et ne suit plus les en-têtes (un .cpp
    # n'était pas recompilé quand un .hpp changeait). CMakePresets.json fait donc parler MSVC en anglais (VSLANG=1033)
    # et donne le préfixe anglais.
    add_compile_options(/utf-8 /Zc:__cplusplus)
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
