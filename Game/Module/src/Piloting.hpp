#pragma once

#include <Flux/Scene/PlayerCommand.hpp>
#include <Flux/Scene/Ships.hpp>

namespace Flux {
    class Engine;
    class InputActions;
}

// Commandes du vaisseau piloté par le joueur (sorties du moteur le 2026-11-01) : touches du vaisseau (frein, boost,
// mode voyage, monter, descendre, roulis, stabiliser à plat) et des déplacements (avancer, glisser), lues dans sa
// commande (PlayerCommand) et données au moteur (Engine::SetPilotInput) qui fait voler le vaisseau (Ships::Update).
namespace Game {
    // Actions du pilote, par leur bit dans PlayerCommand ; avancer, reculer et glisser sont celles du moteur.
    struct PilotActions
    {
        Flux::InputActionId forward = Flux::NoInputAction;
        Flux::InputActionId back = Flux::NoInputAction;
        Flux::InputActionId left = Flux::NoInputAction;
        Flux::InputActionId right = Flux::NoInputAction;
        Flux::InputActionId up = Flux::NoInputAction;
        Flux::InputActionId down = Flux::NoInputAction;
        Flux::InputActionId rollLeft = Flux::NoInputAction;
        Flux::InputActionId rollRight = Flux::NoInputAction;
        Flux::InputActionId brake = Flux::NoInputAction;
        Flux::InputActionId boost = Flux::NoInputAction;
        Flux::InputActionId travel = Flux::NoInputAction;
        Flux::InputActionId level = Flux::NoInputAction;
    };
    // Déclare les actions du vaisseau (après celles du moteur) et rend leurs identifiants.
    PilotActions DeclarePilotActions(Flux::InputActions& actions);
    void RemovePilotActions(Flux::InputActions& actions);
    // Poussée, glissement, roulis, frein, boost et mode voyage ; le moteur y ajoute le manche et les environs.
    Flux::ShipInput ReadShipInput(const Flux::PlayerCommand& command, const PilotActions& actions);

    void RegisterPiloting(Flux::Engine& engine);
    void UnregisterPiloting(Flux::Engine& engine);
}
