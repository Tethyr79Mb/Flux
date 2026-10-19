#pragma once

namespace Flux {
    class Engine;
}

// Combat du jeu : les vaisseaux ennemis (CombatAIComponent) visent le vaisseau du joueur. Le moteur fournit l'IA
// (CombatAI::Steer, CombatAI::Fire) ; le jeu choisit la cible et l'appelle au bon moment de l'image.
namespace Game {
    void RegisterCombat(Flux::Engine& engine);
    void UnregisterCombat(Flux::Engine& engine);
}
