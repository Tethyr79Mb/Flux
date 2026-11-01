#pragma once

namespace Flux {
    class Engine;
}

// Commandes du vaisseau piloté par le joueur (sorties du moteur le 2026-11-01) : touches du vaisseau (frein, boost,
// mode voyage, monter, descendre, roulis, stabiliser à plat) et des déplacements (avancer, glisser), données au moteur
// (Engine::SetPilotInput) qui fait voler le vaisseau (Ships::Update).
namespace Game {
    void RegisterPiloting(Flux::Engine& engine);
    void UnregisterPiloting(Flux::Engine& engine);
}
