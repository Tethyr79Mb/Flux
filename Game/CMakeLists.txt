# Code du jeu : composants et systèmes. Chargé en DLL par l'éditeur et par le client (Game.exe).
add_library(GameModule SHARED
        Module/src/Game.cpp
)
set_target_properties(GameModule PROPERTIES PREFIX "")
target_link_libraries(GameModule PRIVATE Flux::Engine)
flux_set_warnings(GameModule)

# Scènes et autres données du jeu, copiées dans bin/assets/ à côté de celles du moteur.
flux_add_assets(GameAssets Module/assets)
add_dependencies(GameModule GameAssets)

# Client : l'exe du jeu, sans éditeur (Game.exe). Le nom Launcher est réservé au futur lanceur
# (mises à jour, paramètres de lancement), qui démarrera Game.exe.
add_executable(Game Client/src/main.cpp)
target_link_libraries(Game PRIVATE Flux::Engine)
target_compile_definitions(Game PRIVATE FLUX_GAME_MODULE_FILE="$<TARGET_FILE_NAME:GameModule>")
add_dependencies(Game GameModule)
flux_set_warnings(Game)
