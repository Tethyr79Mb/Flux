# Tests automatiques du moteur : un exe par groupe, lancé par ctest (ou directement depuis CLion).
add_executable(FluxTests src/SceneTests.cpp)
target_link_libraries(FluxTests PRIVATE Flux::Engine)
flux_set_warnings(FluxTests)
add_test(NAME Scene COMMAND FluxTests WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
