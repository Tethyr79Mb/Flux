#pragma once

namespace Flux {
    class Simulation;
}

// Combat du jeu : les vaisseaux ennemis (CombatAIComponent) visent le vaisseau du joueur. L'IA est dans
// CombatAI.hpp (CombatAI::Steer, CombatAI::Fire) ; ici, le choix de la cible, à chaque pas.
namespace Game::Combat {
    // Étape BeforePhysics : les ennemis manœuvrent.
    void Steer(Flux::Simulation& simulation, double deltaSeconds);
    // Étape AfterPhysics : ils tirent.
    void Fire(Flux::Simulation& simulation, double deltaSeconds);
}
