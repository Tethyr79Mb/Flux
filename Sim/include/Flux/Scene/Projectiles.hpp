#pragma once

#include "Flux/SimExport.hpp"
#include "Flux/Math/WorldPosition.hpp"
#include "Flux/Scene/Components.hpp"

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace Flux {
    class Scene;

    // Un projectile en vol : pas une entité (des centaines par seconde en combat), juste un point qui avance tout
    // droit, dessiné en traçante (TracerRenderer). Le jeu les tire (ses armes) ; le moteur les fait avancer, toucher
    // et les dessine.
    struct Projectile
    {
        Math::WorldPosition position;
        Math::Vector3f velocity; // m/s, monde
        float life = 0.0f;       // s restantes avant de disparaître
        float age = 0.0f;        // s depuis le tir (la traçante ne dépasse pas en arrière de la bouche du canon)
        float damage = 0.0f;
        Math::LinearColor color;
        Entity ship = NullEntity; // vaisseau qui l'a tiré : il ne se touche pas lui-même
        // Missile : la cible qu'il poursuit (NullEntity : tout droit, comme un obus) et son virage le plus serré.
        bool missile = false;
        Entity target = NullEntity;
        float guidance = 0.0f; // rad/s
    };

    // Projectile arrivé sur un corps, pendant Projectiles::Update : de quoi retirer des points de vie (le jeu) et
    // montrer l'impact.
    struct ProjectileHit
    {
        Entity entity = NullEntity; // entité du corps touché
        Math::WorldPosition position;
        float damage = 0.0f;
        Math::LinearColor color;
        bool missile = false;
    };

    // Éclat lumineux bref à l'endroit d'un impact, dessiné avec les traçantes.
    struct Impact
    {
        Math::WorldPosition position;
        Math::LinearColor color;
        float life = 0.0f; // s restantes
        float size = 1.0f; // largeur par rapport à celle d'un impact d'obus (explosion de missile : plus grand)
    };

    // Premier corps sur le segment `from` -> `from + segment` (PhysicsWorld::CastRay), sans toucher `ship` : entité
    // et part du segment parcourue.
    using ProjectileCast = std::function<std::optional<std::pair<Entity, float>>(const Math::WorldPosition& from,
                                                                                 const Math::Vector3f& segment, Entity ship)>;

    namespace Projectiles {
        // Les missiles tournent vers leur cible (où elle sera quand ils l'atteindront, si c'est un vaisseau qui vole),
        // sans virer plus serré que leur guidage. Une cible détruite : ils continuent tout droit. Avant Update.
        FLUX_SIM_API void GuideMissiles(const Scene& scene, std::vector<Projectile>& projectiles, double deltaSeconds);

        // Les projectiles avancent ; ceux en fin de course sont retirés (sans garder l'ordre ni réallouer). Avec
        // `cast`, chacun lance son segment de l'image : s'il touche un corps, il s'arrête et rejoint `hits`.
        FLUX_SIM_API void Update(std::vector<Projectile>& projectiles, double deltaSeconds, const ProjectileCast& cast = {},
                             std::vector<ProjectileHit>* hits = nullptr);

        // Les éclats d'impact s'éteignent.
        FLUX_SIM_API void UpdateImpacts(std::vector<Impact>& impacts, double deltaSeconds);
    }
}
