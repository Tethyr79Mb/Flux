#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/WorldPosition.hpp"
#include "Flux/Scene/Components.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
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
        Entity ship = NullEntity; // vaisseau qui l'a tiré : il ne se touche pas lui-même
    };

    // Projectile arrivé sur un corps, pendant Weapons::UpdateProjectiles : de quoi retirer des points de vie et
    // montrer l'impact.
    struct ProjectileHit
    {
        Entity entity = NullEntity; // entité du corps touché
        Math::WorldPosition position;
        float damage = 0.0f;
        Math::LinearColor color;
    };

    // Éclat lumineux bref à l'endroit d'un impact, dessiné avec les traçantes.
    struct Impact
    {
        Math::WorldPosition position;
        Math::LinearColor color;
        float life = 0.0f; // s restantes
    };

    // Premier corps sur le segment `from` -> `from + segment` (PhysicsWorld::CastRay), sans toucher `ship` : entité
    // et part du segment parcourue.
    using ProjectileCast = std::function<std::optional<std::pair<Entity, float>>(const Math::WorldPosition& from,
                                                                                 const Math::Vector3f& segment, Entity ship)>;

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

        // Les projectiles avancent ; ceux en fin de course sont retirés (sans garder l'ordre ni réallouer). Avec
        // `cast`, chacun lance son segment de l'image : s'il touche un corps, il s'arrête et rejoint `hits`.
        FLUX_API void UpdateProjectiles(std::vector<Projectile>& projectiles, double deltaSeconds, const ProjectileCast& cast = {},
                                        std::vector<ProjectileHit>* hits = nullptr);

        // Cible d'un corps touché : l'entité elle-même ou son ancêtre le plus proche qui a des points de vie.
        FLUX_API Entity FindTarget(const Scene& scene, Entity entity);
        // Retire les dégâts des impacts aux cibles. Celles qui tombent à 0 sont détruites avec leurs enfants, sauf
        // si elles portent `protectedEntity` (le joueur) ; leurs noms sont ajoutés à `destroyed` s'il est donné.
        FLUX_API void ApplyHits(Scene& scene, std::span<const ProjectileHit> hits, Entity protectedEntity,
                                std::vector<std::string>* destroyed = nullptr);
        // Les éclats d'impact s'éteignent.
        FLUX_API void UpdateImpacts(std::vector<Impact>& impacts, double deltaSeconds);
    }
}
