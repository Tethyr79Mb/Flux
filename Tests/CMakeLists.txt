# Tests de la simulation (FluxSim) : Windows et Linux (CI .github/workflows/linux.yml).
add_executable(FluxSimTests)
flux_add_sources(FluxSimTests Sim)
target_link_libraries(FluxSimTests PRIVATE Flux::Sim meshoptimizer) # meshoptimizer : modèles compressés
# En-têtes internes de FluxSim (semis des arbres, bibliothèque de végétation).
target_include_directories(FluxSimTests PRIVATE ${PROJECT_SOURCE_DIR}/Sim/src)
# Les biomes livrés avec le jeu sont lus là où ils sont rangés (VegetationTests.cpp).
target_compile_definitions(FluxSimTests PRIVATE FLUX_GAME_ASSETS="${PROJECT_SOURCE_DIR}/Game/Module/assets")
flux_set_warnings(FluxSimTests)
flux_precompile_headers(FluxSimTests)
add_test(NAME FluxSimTests COMMAND FluxSimTests WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

if(NOT FLUX_BUILD_ENGINE)
    return()
endif()

# Tests du moteur et du jeu (rendu, interface, son…), dans un exe lancé par ctest.
add_executable(FluxTests)
flux_add_sources(FluxTests src)
target_link_libraries(FluxTests PRIVATE Flux::Engine GameLogic bc7enc) # GameLogic : code du jeu (tests dans src/Game) # bc7enc : décodeur BC7 des tests
# Client réseau du test du serveur HTTP local (LocalHttpServerTests.cpp).
target_link_libraries(FluxTests PRIVATE ws2_32)
# Les tests du RHI (calculs sur le GPU) voient les en-têtes internes du moteur ; seul RHI::Device::Create* est exporté.
target_include_directories(FluxTests PRIVATE ${PROJECT_SOURCE_DIR}/Engine/src ${PROJECT_SOURCE_DIR}/Sim/src)
flux_add_shaders(FluxTestShaders shaders/ComputeTests.slang shaders/CloudNoiseTests.slang shaders/TransferTests.slang
                  shaders/EnvironmentTests.slang)
add_dependencies(FluxTests FluxTestShaders)
flux_set_warnings(FluxTests)
flux_precompile_headers(FluxTests)
add_test(NAME FluxTests COMMAND FluxTests WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
