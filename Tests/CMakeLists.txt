# Tests automatiques du moteur, tous dans un exe lancé par ctest (ou directement depuis CLion).
add_executable(FluxTests
        src/main.cpp
        src/PickingTests.cpp
        src/ProjectTests.cpp
        src/SceneTests.cpp
        src/SceneSerializerTests.cpp
        src/UiTests.cpp
)
target_link_libraries(FluxTests PRIVATE Flux::Engine)
flux_set_warnings(FluxTests)
add_test(NAME FluxTests COMMAND FluxTests WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
