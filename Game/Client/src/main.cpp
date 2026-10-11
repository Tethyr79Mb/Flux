#include <Flux/Core/GameClient.hpp>

// Le moteur fait tout (projet, module du jeu, boucle) : voir Flux::RunGame. Le nom du module vient de CMake.
int main() {
    return Flux::RunGame(FLUX_GAME_MODULE_FILE);
}
