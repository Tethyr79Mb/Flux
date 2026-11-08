# FluxNet : le réseau du jeu (transport, session, vérification des versions), utilisé par le client et par le serveur.
# Dépend de FluxSim (journal), jamais du rendu. Bibliothèque partagée en développement (FluxNet.dll, libFluxNet.so) :
# modifier son intérieur ne recompile ni le moteur ni le jeu. Son interface publique (include/Flux/Net) reste petite et
# stable ; GameNetworkingSockets, protobuf et abseil y sont liés en statique et n'apparaissent dans aucun en-tête.
add_library(FluxNet SHARED)
flux_add_sources(FluxNet src include)
add_library(Flux::Net ALIAS FluxNet)

if(WIN32)
    set_target_properties(FluxNet PROPERTIES PREFIX "") # FluxNet.dll plutôt que libFluxNet.dll avec MinGW
endif()

include(GenerateExportHeader)
generate_export_header(FluxNet
        BASE_NAME FLUX_NET
        EXPORT_MACRO_NAME FLUX_NET_API
        EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/include/Flux/NetExport.hpp)

target_include_directories(FluxNet
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_BINARY_DIR}/include
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(FluxNet PUBLIC Flux::Sim PRIVATE GameNetworkingSockets::static)
if(WIN32)
    target_compile_definitions(FluxNet PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    target_link_options(FluxNet PRIVATE -Wl,--no-undefined)
endif()

flux_set_warnings(FluxNet)
