#pragma once

#include <Flux/Scene/Scene.hpp>

#include <vector>

namespace Flux {
    class Simulation;
}

// Combat du jeu : les vaisseaux ennemis (CombatAIComponent) visent le vaisseau du joueur le plus proche. L'IA est dans
// CombatAI.hpp (CombatAI::Steer, CombatAI::Fire) ; ici, les cibles possibles, à chaque pas.
namespace Game::Combat {
    // Étape BeforePhysics : les ennemis manœuvrent. targets : mémoire gardée d'un pas à l'autre.
    void Steer(Flux::Simulation& simulation, std::vector<Flux::Entity>& targets, double deltaSeconds);
    // Étape AfterPhysics : ils tirent.
    void Fire(Flux::Simulation& simulation, double deltaSeconds);
}
