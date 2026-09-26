add_executable(FluxEditor
        src/main.cpp
        src/SceneDocument.cpp
        src/SceneEditor.cpp
)
target_link_libraries(FluxEditor PRIVATE Flux::Engine)

# L'éditeur ne lie pas le jeu : il charge sa DLL à l'exécution pour pouvoir la recharger.
target_compile_definitions(FluxEditor PRIVATE FLUX_GAME_MODULE_FILE="$<TARGET_FILE_NAME:GameModule>")
add_dependencies(FluxEditor GameModule)
# Dossier proposé par « Ouvrir » et « Enregistrer sous » : les scènes du dépôt, pas leur copie dans bin/.
target_compile_definitions(FluxEditor PRIVATE FLUX_SOURCE_SCENES_DIR="${PROJECT_SOURCE_DIR}/Game/Module/assets/scenes")

flux_set_warnings(FluxEditor)
