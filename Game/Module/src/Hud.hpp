#pragma once

#include <memory>

namespace Flux {
    class Engine;
}

// Affichages du jeu sur le client, d'après l'état de sa simulation (GameSim.hpp) : destinations du warp, missions,
// verrou des armes. Chacun lit l'état à chaque image ; il ne change la simulation que par les fonctions de GameSim
// (accepter une mission...).
namespace Game {
    struct SimState;

    void RegisterNavigationHud(Flux::Engine& engine, std::shared_ptr<SimState> state);
    void UnregisterNavigationHud(Flux::Engine& engine);
    // Aussi la mission en cours dans la sauvegarde rapide.
    void RegisterMissionsHud(Flux::Engine& engine, std::shared_ptr<SimState> state);
    void UnregisterMissionsHud(Flux::Engine& engine);
    // Le verrou est sur le personnage du joueur (PilotWeaponsComponent).
    void RegisterPlayerWeaponsHud(Flux::Engine& engine);
    void UnregisterPlayerWeaponsHud(Flux::Engine& engine);
}
