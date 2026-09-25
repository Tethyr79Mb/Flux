# Code du jeu : composants et systèmes. Chargé en DLL par l'éditeur et par le launcher.
add_library(GameModule SHARED
        Module/src/Game.cpp
)
set_target_properties(GameModule PROPERTIES PREFIX "")
target_link_libraries(GameModule PRIVATE Flux::Engine)
flux_set_warnings(GameModule)

# Exe du jeu, sans éditeur.
add_executable(Game Launcher/src/main.cpp)
target_link_libraries(Game PRIVATE Flux::Engine)
target_compile_definitions(Game PRIVATE FLUX_GAME_MODULE_FILE="$<TARGET_FILE_NAME:GameModule>")
add_dependencies(Game GameModule)
flux_set_warnings(Game)
