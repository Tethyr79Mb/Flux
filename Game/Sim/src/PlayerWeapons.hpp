#pragma once

#include <Flux/Scene/Components.hpp>
#include <Flux/Scene/PlayerCommand.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace Flux {
    class Scene;
    class Simulation;
}

// Armes du joueur : touches des groupes (1 à 0), verrouillage de la cible au centre de la vue, tir automatique. Les
// armes sont dans Weapons.cpp, la simulation fait voler les projectiles ; ici : quand et sur quoi le joueur tire. Les
// coins autour de la cible sont dessinés par le client (PlayerWeaponsHud.cpp).
namespace Game {
    struct SimState;

    // Armes d'un joueur, sur son personnage : chacun a son verrou et ses groupes actifs. Le temps du jeu seulement (ni
    // inspecteur ni sauvegarde) : un chargement repart sans verrou.
    struct PilotWeaponsComponent
    {
        Flux::Entity lockTarget = Flux::NullEntity; // cible verrouillée par les armes du vaisseau piloté
        std::uint16_t activeGroups = 0;             // groupes en tir automatique (bit 0 : touche 1)
    };

    struct PlayerWeaponsState
    {
        std::array<Flux::InputActionId, 10> groupActions{}; // bits des groupes dans PlayerCommand
        std::string info; // lignes des infos de vol (groupes actifs, missiles), gardée d'un pas à l'autre
    };

    namespace PlayerWeapons {
        // Actions des groupes d'armes (WeaponComponent::group), sur les chiffres au-dessus des lettres (choix de
        // Bruno) : sur le Chasseur, 1 le nez, 2 les autocanons, 3 les missiles.
        inline constexpr std::array<std::string_view, 10> GroupActions{
            "Groupe d'armes 1", "Groupe d'armes 2", "Groupe d'armes 3", "Groupe d'armes 4", "Groupe d'armes 5",
            "Groupe d'armes 6", "Groupe d'armes 7", "Groupe d'armes 8", "Groupe d'armes 9", "Groupe d'armes 10"};

        void DeclareActions(PlayerWeaponsState& state, Flux::CommandActions& actions);
        // Étape AfterPhysics, après les dégâts et avant les ennemis : les armes du vaisseau que pilote le joueur.
        void Fire(Flux::Simulation& simulation, SimState& state, double deltaSeconds);
        // Verrou et groupes actifs du joueur, nullptr s'il n'en a pas.
        const PilotWeaponsComponent* FindState(const Flux::Scene& scene, Flux::Entity player);
        // Scène remplacée : verrous oubliés.
        void Reset(Flux::Scene& scene, PlayerWeaponsState& state);
    }
}
