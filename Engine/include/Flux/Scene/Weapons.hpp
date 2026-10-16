#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/WorldPosition.hpp"
#include "Flux/Scene/Components.hpp"

#include <cstdint>
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
        // Missile : la cible qu'il poursuit (NullEntity : tout droit, comme un obus) et son virage le plus serré.
        bool missile = false;
        Entity target = NullEntity;
        float guidance = 0.0f; // rad/s
    };

    // Projectile arrivé sur un corps, pendant Weapons::UpdateProjectiles : de quoi retirer des points de vie et
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

    // Commandes du pilote pour les armes de son vaisseau.
    struct WeaponInput
    {
        // Groupes d'armes actifs (bit 0 : groupe 1, ... bit 9 : groupe 10 ; touches 1 à 0). Les canons d'un groupe actif
        // tirent seuls ; un lance-missiles d'un groupe actif lance un missile (voir Update). Rien ne part sans cible
        // verrouillée à la portée de l'arme.
        std::uint16_t active = 0;
        bool aim = false;  // aimPoint donné : les tourelles s'y tournent ; sinon elles reviennent vers l'avant
        Math::WorldPosition aimPoint;
        Entity target = NullEntity; // cible verrouillée (FindLockTarget) : visée en avance, poursuivie par les missiles
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
        // Ships::Update (le vaisseau a sa vitesse de cette image). Au plus un missile part par image : retourne le
        // groupe (bit) qui l'a lancé, 0 sinon, pour que l'appelant désactive ce groupe (un appui, un missile).
        FLUX_API std::uint16_t Update(Scene& scene, Entity ship, double deltaSeconds, const WeaponInput& input,
                             std::vector<Projectile>& projectiles);

        // Cible à verrouiller quand le pilote appuie sur une touche de tir : celle qui a des points de vie, à portée des
        // armes du vaisseau, la plus proche de la direction regardée depuis `from` sans s'en écarter de plus de leur
        // angle de verrouillage. NullEntity sans arme montée, ou si rien n'est dans le viseur. Ni le vaisseau, ni ce
        // qui le porte.
        FLUX_API Entity FindLockTarget(const Scene& scene, Entity ship, const Math::WorldPosition& from, const Math::Vector3f& direction);
        // Le verrou tient tant que la cible existe, a des points de vie et reste à portée des armes du vaisseau (elle
        // peut sortir du viseur).
        FLUX_API bool KeepsLock(const Scene& scene, Entity ship, Entity target, const Math::WorldPosition& from);
        // Missiles restants dans les lance-missiles du vaisseau ; -1 s'ils sont illimités, 0 sans lance-missiles.
        FLUX_API int GetMissileCount(const Scene& scene, Entity ship);
        // Les missiles tournent vers leur cible (où elle sera quand ils l'atteindront, si c'est un vaisseau qui vole),
        // sans virer plus serré que leur guidage. Une cible détruite : ils continuent tout droit. Avant
        // UpdateProjectiles.
        FLUX_API void GuideMissiles(const Scene& scene, std::vector<Projectile>& projectiles, double deltaSeconds);

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
