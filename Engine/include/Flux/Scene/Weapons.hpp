#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/WorldPosition.hpp"
#include "Flux/Scene/Components.hpp"

#include <string>
#include <vector>

namespace Flux {
    class Scene;

    // Un projectile en vol : pas une entité (des centaines par seconde en combat), juste un point qui avance tout
    // droit, dessiné en traçante (TracerRenderer).
    struct Projectile
    {
        Math::WorldPosition position;
        Math::Vector3f velocity; // m/s, monde
        float life = 0.0f;       // s restantes avant de disparaître
        float age = 0.0f;        // s depuis le tir (la traçante ne dépasse pas en arrière de la bouche du canon)
        float damage = 0.0f;
        Math::LinearColor color;
        Entity ship = NullEntity; // vaisseau qui l'a tiré : il ne se touche pas lui-même (dégâts : étape suivante)
    };

    // Commandes du pilote pour les armes de son vaisseau.
    struct WeaponInput
    {
        bool fire = false; // action « Tirer » tenue
        bool aim = false;  // aimPoint donné : les tourelles s'y tournent ; sinon elles reviennent vers l'avant
        Math::WorldPosition aimPoint;
    };

    // Armes des vaisseaux : une arme (WeaponComponent) n'est active que posée sur un emport (HardpointComponent, son
    // parent direct) qui l'accepte (Fits), dans le vaisseau piloté.
    namespace Weapons {
        // L'emport accepte l'arme : même genre, pas plus grande que lui.
        FLUX_API bool Fits(const HardpointComponent& hardpoint, const WeaponComponent& weapon);
        // Pourquoi l'emport refuse l'arme (texte pour l'éditeur), vide s'il l'accepte.
        FLUX_API std::string GetRefusal(const HardpointComponent& hardpoint, const WeaponComponent& weapon);

        // Vaisseau piloté (NullEntity : aucun) : ses armes pivotent vers le point visé et tirent. Les projectiles
        // tirés sont ajoutés à `projectiles`. Les armes des autres vaisseaux ne bougent pas. Pendant le jeu, après
        // Ships::Update (le vaisseau a sa vitesse de cette image).
        FLUX_API void Update(Scene& scene, Entity ship, double deltaSeconds, const WeaponInput& input,
                             std::vector<Projectile>& projectiles);

        // Les projectiles avancent ; ceux en fin de course sont retirés (sans garder l'ordre ni réallouer).
        FLUX_API void UpdateProjectiles(std::vector<Projectile>& projectiles, double deltaSeconds);
    }
}
