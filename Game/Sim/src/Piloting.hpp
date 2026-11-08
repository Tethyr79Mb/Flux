#pragma once

#include <Flux/Scene/PlayerCommand.hpp>
#include <Flux/Scene/Ships.hpp>

#include <array>
#include <string_view>

namespace Flux {
    class Simulation;
}

// Commandes du vaisseau piloté par le joueur (sorties du moteur le 2026-11-01) : touches du vaisseau (frein, boost,
// mode voyage, monter, descendre, roulis, stabiliser à plat) et des déplacements (avancer, glisser), lues dans sa
// commande (PlayerCommand) et données à la simulation (Simulation::SetPilotInput), qui fait voler le vaisseau.
namespace Game {
    // Actions du pilote, par leur bit dans PlayerCommand ; avancer, reculer et glisser sont celles du personnage.
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

    namespace Piloting {
        // Noms des actions propres au vaisseau, dans l'ordre de déclaration (leurs touches sont au client).
        inline constexpr std::string_view Brake = "Freiner";
        inline constexpr std::string_view Boost = "Boost";
        inline constexpr std::string_view Travel = "Mode voyage";
        inline constexpr std::string_view Up = "Monter";
        inline constexpr std::string_view Down = "Descendre";
        inline constexpr std::string_view RollLeft = "Rouler à gauche";
        inline constexpr std::string_view RollRight = "Rouler à droite";
        inline constexpr std::string_view Level = "Stabiliser à plat";
        inline constexpr std::array<std::string_view, 8> ShipActions{Brake, Boost, Travel, Up, Down, RollLeft, RollRight, Level};

        // Déclare les actions du vaisseau (après celles du personnage) et rend leurs bits.
        PilotActions DeclareActions(Flux::CommandActions& actions);
        // Poussée, glissement, roulis, frein, boost et mode voyage ; la simulation y ajoute le manche et les environs.
        Flux::ShipInput ReadShipInput(const Flux::PlayerCommand& command, const PilotActions& actions);
        // Étape Interaction (avant les sièges) : assis aux commandes, les touches du joueur pilotent.
        void Pilot(Flux::Simulation& simulation, const PilotActions& actions);
    }
}
