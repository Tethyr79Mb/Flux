cmake_minimum_required(VERSION 3.28)
# Numéro de version du moteur et du jeu, à changer à chaque sortie (le commit Git s'y ajoute : cmake/FluxVersion.cmake).
project(Flux VERSION 0.1.0 LANGUAGES C CXX) # C : requis par volk

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Toutes les DLL à côté des exe : Windows ne les trouve pas ailleurs. Avec les préréglages de Visual Studio, ce dossier
# est Flux/bin/<préréglage> : le jeu prêt à lancer, à part des fichiers intermédiaires de out/.
set(FLUX_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin CACHE PATH "Dossier des exe, DLL, shaders et assets")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${FLUX_OUTPUT_DIRECTORY})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${FLUX_OUTPUT_DIRECTORY})
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

# Processeur minimum : AVX2 (Intel Haswell 2013, AMD Ryzen 2017), pour tout le projet et ses dépendances (décision de
# Bruno du 2026-10-28). Jolt ne le met lui-même qu'avec le générateur Visual Studio, pas avec Ninja : il restait en SSE2.
# Pas de multiplication-addition fusionnée implicite (FMA) : elle changerait les doubles du relief, que PlanetGpuTests
# compare au shader. MSVC n'en génère pas sans /fp:contract ; GCC, lui, est forcé avec -ffp-contract=off.
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/arch:AVX2>)
else()
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-march=x86-64-v3> $<$<COMPILE_LANGUAGE:C,CXX>:-ffp-contract=off>)
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
include(cmake/FluxVersion.cmake)

add_subdirectory(Engine)
add_subdirectory(Game)
add_subdirectory(Editor)

enable_testing()
add_subdirectory(Tests)
