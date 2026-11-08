# Code du jeu, en deux bibliothèques statiques :
# - GameSim (Sim/src) : ce qui se simule (composants, pilotage, navigation, missions, armes, combat), FluxSim seule.
#   Compilée aussi sous Linux : c'est le jeu que fera tourner le serveur (Game/Server).
# - GameClient (Module/src) : ce que voit et touche le joueur (touches, affichages, interfaces, objets de l'éditeur).
# GameModule.dll (chargée par l'éditeur et par Ars Solaris.exe) n'a que ses points d'entrée (Module/Dll) ; FluxTests
# lie GameClient pour tester le jeu.
add_library(GameSim STATIC)
flux_add_sources(GameSim Sim/src)
target_include_directories(GameSim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Sim/src)
target_link_libraries(GameSim PUBLIC Flux::Sim)
# Liée dans GameModule.dll (et un jour dans un module .so) : code relogeable.
set_target_properties(GameSim PROPERTIES POSITION_INDEPENDENT_CODE ON)
flux_set_warnings(GameSim)

if(NOT FLUX_BUILD_ENGINE)
    return()
endif()

add_library(GameClient STATIC)
flux_add_sources(GameClient Module/src)
target_include_directories(GameClient PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Module/src)
target_link_libraries(GameClient PUBLIC GameSim Flux::Engine)
flux_set_warnings(GameClient)

add_library(GameModule SHARED)
flux_add_sources(GameModule Module/Dll)
set_target_properties(GameModule PROPERTIES PREFIX "")
target_link_libraries(GameModule PRIVATE GameClient)
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
