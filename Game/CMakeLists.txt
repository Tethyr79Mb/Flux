# Code du jeu : composants et systèmes. Chargé en DLL par l'éditeur et par le client (Ars Solaris.exe).
add_library(GameModule SHARED)
flux_add_sources(GameModule Module/src)
set_target_properties(GameModule PROPERTIES PREFIX "")
target_link_libraries(GameModule PRIVATE Flux::Engine)
flux_set_warnings(GameModule)

# Scènes et autres données du jeu, copiées dans bin/assets/ à côté de celles du moteur.
flux_add_assets(GameAssets Module/assets)
add_dependencies(GameModule GameAssets)

# Le projet (nom du jeu, scène de démarrage), à côté de l'exe du jeu : ses chemins relatifs y restent valides.
set(gameProjectOutput ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Game.fluxproject)
add_custom_command(
        OUTPUT ${gameProjectOutput}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/Module/Game.fluxproject ${gameProjectOutput}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Module/Game.fluxproject
        COMMENT "Copie de Game.fluxproject"
        VERBATIM)
add_custom_target(GameProject DEPENDS ${gameProjectOutput} SOURCES Module/Game.fluxproject)
add_dependencies(GameModule GameProject)

# Client : l'exe du jeu, sans éditeur (« Ars Solaris.exe »). Le nom Launcher est réservé au futur lanceur
# (mises à jour, paramètres de lancement), qui démarrera cet exe. La cible CMake reste Game ; le fichier porte le nom
# du jeu parce que les pilotes (AMD Adrenalin…) reconnaissent les jeux à leur exe et ignoraient un « Game.exe ».
add_executable(Game)
set_target_properties(Game PROPERTIES OUTPUT_NAME "Ars Solaris")
flux_add_sources(Game Client/src)
target_link_libraries(Game PRIVATE Flux::Engine)
target_compile_definitions(Game PRIVATE FLUX_GAME_MODULE_FILE="$<TARGET_FILE_NAME:GameModule>")
add_dependencies(Game GameModule)
flux_set_warnings(Game)
