add_executable(FluxEditor
        src/main.cpp
        src/SceneEditor.cpp
)
target_link_libraries(FluxEditor PRIVATE Flux::Engine)

# L'éditeur ne lie pas le jeu : il charge sa DLL à l'exécution pour pouvoir la recharger.
target_compile_definitions(FluxEditor PRIVATE FLUX_GAME_MODULE_FILE="$<TARGET_FILE_NAME:GameModule>")
add_dependencies(FluxEditor GameModule)

flux_set_warnings(FluxEditor)
