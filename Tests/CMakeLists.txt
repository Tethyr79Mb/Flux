# Tests automatiques du moteur, tous dans un exe lancé par ctest (ou directement depuis CLion).
add_executable(FluxTests)
flux_add_sources(FluxTests src)
target_link_libraries(FluxTests PRIVATE Flux::Engine)
flux_set_warnings(FluxTests)
flux_precompile_headers(FluxTests)
add_test(NAME FluxTests COMMAND FluxTests WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
