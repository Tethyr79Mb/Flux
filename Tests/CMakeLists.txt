# Tests automatiques du moteur, tous dans un exe lancé par ctest (ou directement depuis CLion).
add_executable(FluxTests)
flux_add_sources(FluxTests src)
target_link_libraries(FluxTests PRIVATE Flux::Engine bc7enc meshoptimizer) # bc7enc : décodeur BC7 des tests ; meshoptimizer : modèles compressés
# Les tests du RHI (calculs sur le GPU) voient les en-têtes internes du moteur ; seul RHI::Device::Create* est exporté.
target_include_directories(FluxTests PRIVATE ${PROJECT_SOURCE_DIR}/Engine/src)
# Les biomes livrés avec le jeu sont lus là où ils sont rangés (VegetationTests.cpp).
target_compile_definitions(FluxTests PRIVATE FLUX_GAME_ASSETS="${PROJECT_SOURCE_DIR}/Game/Module/assets")
flux_add_shaders(FluxTestShaders shaders/ComputeTests.slang shaders/CloudNoiseTests.slang shaders/TransferTests.slang)
add_dependencies(FluxTests FluxTestShaders)
flux_set_warnings(FluxTests)
flux_precompile_headers(FluxTests)
add_test(NAME FluxTests COMMAND FluxTests WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
