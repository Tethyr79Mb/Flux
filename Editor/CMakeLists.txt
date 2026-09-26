add_executable(FluxEditor
        src/main.cpp
        src/ContentBrowser.cpp
        src/PlayMode.cpp
        src/SceneDocument.cpp
        src/SceneEditor.cpp
)
target_link_libraries(FluxEditor PRIVATE Flux::Engine)

# L'éditeur ne lie pas le jeu : il charge sa DLL à l'exécution pour pouvoir la recharger.
target_compile_definitions(FluxEditor PRIVATE FLUX_GAME_MODULE_FILE="$<TARGET_FILE_NAME:GameModule>")
add_dependencies(FluxEditor GameModule)
# Projet ouvert par défaut (sans argument) : celui du dépôt, pour enregistrer les scènes dans les sources
# et non dans leur copie de bin/, écrasée par le build.
target_compile_definitions(FluxEditor PRIVATE FLUX_DEFAULT_PROJECT="${PROJECT_SOURCE_DIR}/Game/Module/Game.fluxproject")

flux_set_warnings(FluxEditor)
