# FluxSim : la simulation, sans rendu, son ni fenêtre (scène EnTT, physique Jolt, planètes côté CPU, modèles, journal).
# Compilée sous Windows (FluxSim.dll, sous FluxEngine) et sous Linux (libFluxSim.so) : c'est la base du futur serveur
# de jeu. Une bibliothèque partagée plutôt que statique : moteur, module du jeu et serveur doivent partager un seul
# registre de composants et un seul journal. Rien ici ne doit inclure Vulkan, FreeType, msdfgen, miniaudio ni Win32
# (hors Platform/Win32) ; la CI Linux le vérifie.
add_library(FluxSim SHARED)
flux_add_sources(FluxSim src include)
add_library(Flux::Sim ALIAS FluxSim)

# FluxSim.dll plutôt que libFluxSim.dll avec MinGW.
if(WIN32)
    set_target_properties(FluxSim PROPERTIES PREFIX "")
endif()

include(GenerateExportHeader)
generate_export_header(FluxSim
        BASE_NAME FLUX_SIM
        EXPORT_MACRO_NAME FLUX_SIM_API
        EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/include/Flux/SimExport.hpp)

target_include_directories(FluxSim
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_BINARY_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(WIN32)
    target_compile_definitions(FluxSim PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_link_libraries(FluxSim PRIVATE shell32) # CommandLineToArgvW (arguments du processus)
else()
    # dlopen (SharedLibrary) ; fils du JobSystem.
    find_package(Threads REQUIRED)
    target_link_libraries(FluxSim PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
    # Un symbole manquant est une erreur dès la compilation de la .so, comme pour une DLL, pas au chargement.
    target_link_options(FluxSim PRIVATE -Wl,--no-undefined)
endif()

# EnTT est public : les modules de jeu manipulent directement la scène.
target_link_libraries(FluxSim PUBLIC EnTT::EnTT)
# Physique (Jolt), privée : ses types n'apparaissent dans aucun en-tête public.
target_link_libraries(FluxSim PRIVATE Jolt)
# Fichiers de scène JSON ; images PNG et JPEG ; modèles glTF et leurs paquets de triangles (meshlets).
target_link_libraries(FluxSim PRIVATE nlohmann_json::nlohmann_json stb_image stb_image_impl cgltf_header meshoptimizer)

flux_set_warnings(FluxSim)
flux_precompile_headers(FluxSim)
