#pragma once

#include <Flux/Math/Color.hpp>
#include <Flux/Scene/PlayerCommand.hpp>

#include <string_view>

namespace Flux {
    class ComponentRegistry;
    class Scene;
    class Simulation;
}

// Arme de poing du personnage à pied (pistolet) : une touche la sort ou la range, le clic droit vise, chaque clic
// gauche tire un coup. L'arme est une entité enfant du personnage, accrochée à l'os de sa main
// (BoneAttachmentComponent) ; rangée, elle n'est pas dessinée. Le graphe d'animation du personnage reçoit armed,
// aiming et fire (sa couche du haut du corps tient, vise et tire) ; les projectiles et les dégâts sont ceux des
// vaisseaux (Weapons::ApplyHits).
namespace Game {
    // Sur l'arme. Son -z : le canon.
    struct HandWeaponComponent
    {
        float fireRate = 4.0f;          // coups par seconde au plus
        float projectileSpeed = 350.0f; // m/s (9 mm)
        float range = 150.0f;           // m : au-delà, le projectile disparaît
        float damage = 20.0f;           // par coup
        float muzzle = 0.12f;           // m, de l'origine de l'arme à la bouche du canon
        Flux::Math::LinearColor color{1.0f, 0.8f, 0.45f}; // traçante
        float cooldown = 0.0f;          // pendant le jeu (non enregistré) : s avant le prochain coup
    };

    // Sur le personnage d'un joueur, le temps du jeu (ni inspecteur ni sauvegarde) : un chargement repart arme rangée.
    struct HandWeaponStateComponent
    {
        bool drawn = false;
    };

    namespace HandWeapons {
        inline constexpr std::string_view DrawAction = "Sortir ou ranger l'arme";
        inline constexpr std::string_view AimAction = "Viser";
        inline constexpr std::string_view FireAction = "Tirer";

        // Paramètres posés sur le graphe du personnage, s'il les déclare : 0 ou 1 ; fire vaut 1 au pas d'un coup.
        inline constexpr std::string_view ArmedParameter = "armed";
        inline constexpr std::string_view AimingParameter = "aiming";
        inline constexpr std::string_view FireParameter = "fire";

        // Les coups visent le point au centre de la vue, à cette distance (m) : ils y croisent l'axe de la caméra.
        inline constexpr float AimDistance = 25.0f;

        struct Actions
        {
            Flux::InputActionId draw = Flux::NoInputAction;
            Flux::InputActionId aim = Flux::NoInputAction;
            Flux::InputActionId fire = Flux::NoInputAction;
        };

        void Register(Flux::ComponentRegistry& registry);
        Actions DeclareActions(Flux::CommandActions& actions);
        // Sortir, ranger, viser et tirer pour chaque joueur, avant l'animation du pas.
        void Update(Flux::Simulation& simulation, const Actions& actions, double deltaSeconds);
        // Fin du jeu : armes de nouveau visibles (comme posées dans l'éditeur), états oubliés.
        void Reset(Flux::Scene& scene);
    }
}
