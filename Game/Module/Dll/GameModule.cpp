// Points d'entrée de GameModule.dll, seul fichier de la DLL : le code du jeu est dans la bibliothèque GameLogic
// (Game/Module/src), que FluxTests lie aussi pour tester le jeu.
#include "Game.hpp"

#include <Flux/Core/GameModule.hpp>

#include <cstdint>

FLUX_GAME_MODULE_EXPORT std::uint32_t FluxGameApiVersion() {
    return Flux::GameModuleApiVersion;
}

FLUX_GAME_MODULE_EXPORT void FluxRegisterGame(Flux::Engine& engine) {
    // Les scènes sont chargées ensuite par l'hôte (Ars Solaris.exe ou l'éditeur) : elles peuvent utiliser ces composants.
    Game::Register(engine);
}

FLUX_GAME_MODULE_EXPORT void FluxUnregisterGame(Flux::Engine& engine) {
    // Rien du jeu ne doit survivre au déchargement de la DLL : ni son code (Update), ni ses entités.
    // La scène, les composants et les objets de l'éditeur enregistrés sont vidés ensuite par le moteur
    // (GameModule::Unregister).
    Game::Unregister(engine);
}
