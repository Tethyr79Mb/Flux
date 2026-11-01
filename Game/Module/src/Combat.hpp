#pragma once

namespace Flux {
    class Engine;
}

// Combat du jeu : les vaisseaux ennemis (CombatAIComponent) visent le vaisseau du joueur. L'IA est dans
// CombatAI.hpp (CombatAI::Steer, CombatAI::Fire) ; ici, le choix de la cible et le moment de l'image.
namespace Game {
    void RegisterCombat(Flux::Engine& engine);
    void UnregisterCombat(Flux::Engine& engine);
}
