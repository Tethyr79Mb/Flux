#pragma once

#include "Flux/Export.hpp"
#include "Flux/Scene/Components.hpp"

namespace Flux {
    class Scene;

    // Commandes du pilote d'un vaisseau (ShipComponent), depuis un poste de pilotage (Seats::GetPilotSeat).
    struct ShipInput
    {
        float thrust = 0.0f;   // Z/S : -1 à 1, vers l'avant
        float strafe = 0.0f;   // Q/D : -1 à 1, glisser vers la droite
        float lift = 0.0f;     // Espace/Ctrl : -1 à 1, monter le long du haut du vaisseau (décoller, se poser)
        float roll = 0.0f;     // A/E : -1 à 1, rouler vers la droite
        bool boost = false;    // Maj : ShipComponent::boost, vers l'avant
        bool travel = false;   // Tab (pressée cette image) : allumer ou éteindre le mode voyage
        bool brake = false;    // X : ralentir jusqu'à l'arrêt
        // Souris : rotation demandée pendant cette image (radians ; lacet vers la gauche, tangage nez vers le haut),
        // ajoutée aux touches ; le vaisseau la suit sans dépasser sa vitesse de rotation.
        float mouseYaw = 0.0f;
        float mousePitch = 0.0f;
        // Haut du lieu où est le vaisseau piloté (dans le monde, unitaire ; opposé à la gravité du dehors), pour
        // ShipComponent::levelAssist ; nul sans gravité (pas d'aide).
        Math::Vector3f up{};
        // Densité de l'air là où il est (0 dans l'espace, 1 au sol d'une atmosphère comme celle de la Terre) :
        // ShipComponent::airGrip.
        float air = 0.0f;
        // Planète la plus proche, à moins de deux rayons : sa verticale ici (unitaire, monde ; nulle loin de toute
        // planète) et la distance à sa limite (haut de l'atmosphère, ou le sol sans atmosphère). En mode voyage, le
        // vaisseau ne s'en approche pas plus vite qu'il ne peut ralentir avant d'y arriver.
        Math::Vector3f planetUp{};
        float planetDistance = 0.0f;
        // Distance au centre de cette planète (0 : inconnue). Avec l'aide à plat, l'assiette et la vitesse du vaisseau
        // suivent la courbure : à plat, il garde son altitude au lieu de piquer peu à peu vers le sol.
        float planetCenter = 0.0f;
    };

    // Vol des vaisseaux : pilotage du vaisseau aux commandes et moteurs des autres. La physique (PhysicsWorld) les
    // déplace ensuite selon ShipComponent::velocity et angularVelocity.
    namespace Ships {
        // piloted : vaisseau aux commandes du joueur (NullEntity : aucun, voir GetPilotedShip). Pendant le jeu, après
        // Seats::Update et avant la physique.
        FLUX_API void Update(Scene& scene, Entity piloted, double deltaSeconds, const ShipInput& input);

        // Vaisseau du poste de pilotage où le joueur est assis, NullEntity sinon.
        FLUX_API Entity GetPilotedShip(const Scene& scene, Entity player);
    }
}
