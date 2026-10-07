#pragma once

#include "Flux/Export.hpp"
#include "Flux/Scene/Components.hpp"

#include <functional>

namespace Flux {
    class Scene;

    // Commandes du joueur pour les sièges (SeatComponent) et le poste de pilotage.
    struct SeatInput
    {
        bool interact = false; // F : s'asseoir sur le siège le plus proche, ou se relever
        float thrust = 0.0f;   // Z/S : -1 à 1, vers l'avant
        float strafe = 0.0f;   // Q/D : -1 à 1, glisser vers la droite
        float lift = 0.0f;     // Espace/Ctrl : -1 à 1, monter le long du haut du vaisseau (décoller, se poser)
        float roll = 0.0f;     // A/E : -1 à 1, rouler vers la droite
        bool boost = false;    // Maj : ShipComponent::boost, vers l'avant
        bool brake = false;    // X : ralentir jusqu'à l'arrêt
        // Souris : rotation demandée pendant cette image (radians ; lacet vers la gauche, tangage nez vers le haut),
        // ajoutée aux touches ; le vaisseau la suit sans dépasser sa vitesse de rotation.
        float mouseYaw = 0.0f;
        float mousePitch = 0.0f;
        // Haut du lieu où est le vaisseau piloté (dans le monde, unitaire ; opposé à la gravité du dehors), pour
        // ShipComponent::levelAssist ; nul sans gravité (pas d'aide).
        Math::Vector3f up{};
        // Densité de l'air là où il est (0 dans l'espace, 1 au sol d'une atmosphère comme celle de la Terre) :
        // ShipComponent::airMaxSpeed et airGrip.
        float air = 0.0f;
    };

    // Le joueur s'assoit, pilote et se relève : voir SeatComponent. La physique pose le personnage sur son siège
    // (CharacterComponent::seat), l'animation suit CharacterComponent::seatState.
    namespace Seats {
        // Durées des animations « s'asseoir » et « se lever » d'un personnage, en secondes (0 : immédiat).
        struct Durations
        {
            float sitDown = 0.0f;
            float standUp = 0.0f;
        };
        using DurationsFunction = std::function<Durations(Entity character)>;

        // player : personnage dirigé par le joueur (NullEntity : aucun). Pendant le jeu, après les commandes et
        // avant la physique.
        FLUX_API void Update(Scene& scene, Entity player, double deltaSeconds, const SeatInput& input,
                             const DurationsFunction& durations);

        // Poste de pilotage où le joueur est assis (étape Seated), NullEntity sinon.
        FLUX_API Entity GetPilotSeat(const Scene& scene, Entity player);
    }
}
