#include <Flux/Core/GameStartup.hpp>

// Le moteur fait tout (réglages, projet, module du jeu, boucle) : voir Flux::GameStartup. Le nom du module vient de
// CMake.
int main() {
    return Flux::GameStartup(FLUX_GAME_MODULE_FILE).Run();
}
