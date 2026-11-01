#pragma once

#include <Flux/Math/WorldPosition.hpp>
#include <Flux/Scene/Components.hpp>
#include <Flux/Scene/Projectiles.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Flux {
    class ComponentRegistry;
    class EditorExtensions;
    class Engine;
    class Scene;
}

// Armes, emports et points de vie (sortis du moteur le 2026-11-01). Le moteur garde les projectiles
// (Flux/Scene/Projectiles.hpp) : il les fait avancer, toucher et les dessine ; le jeu les tire et retire les dégâts.
namespace Game {
    // Ce qu'un emport accepte, et ce qu'est une arme.
    enum class WeaponKind : std::uint8_t {
        Gun,     // canon : tire des projectiles en rafale
        Missile, // lance-missiles
    };

    // Point d'emport d'un vaisseau (« hardpoint ») : l'endroit du modèle où se fixe une arme, son -z vers l'avant du
    // tir. L'arme est une entité enfant (WeaponComponent) : changer d'arme, c'est remplacer cet enfant. Une arme d'un
    // autre genre, ou plus grande que l'emport, n'y tire pas.
    struct HardpointComponent
    {
        WeaponKind kind = WeaponKind::Gun;
        std::int32_t size = 1; // de 1 à 3 : la plus grande arme acceptée
    };

    // Arme posée sur un emport (HardpointComponent parent) d'un vaisseau. Pendant le jeu, aux commandes de ce
    // vaisseau, elle pivote vers le point que regarde le pilote (sans sortir de son cône autour de l'avant de
    // l'emport) et tire tant que la touche de son groupe est tenue (1 à 0 au-dessus des lettres). Son -z : le canon ; ses projectiles partent de
    // muzzle mètres devant son origine.
    struct WeaponComponent
    {
        WeaponKind kind = WeaponKind::Gun;
        std::int32_t size = 1;
        std::int32_t group = 1;         // groupe de tir : touche 1 à 9, 10 pour la touche 0 ; 0 : ne tire pas
        float fireRate = 8.0f;          // tirs par seconde
        float projectileSpeed = 1500.0f; // m/s, en plus de la vitesse du vaisseau
        float range = 2500.0f;          // m : au-delà, le projectile disparaît
        float damage = 10.0f;           // par projectile
        float cone = 20.0f;             // °, débattement autour de l'avant de l'emport (0 : tir fixe)
        float turnRate = 120.0f;        // °/s, vitesse de pivotement
        float muzzle = 1.0f;            // m, de l'origine de l'arme à la bouche du canon
        Flux::Math::LinearColor color{1.0f, 0.55f, 0.2f}; // couleur de la traçante
        // Missiles : nombre emporté (0 : illimité) et virage le plus serré en vol (°/s). Verrouillage : écart autour
        // du centre de la vue dans lequel une cible se verrouille à l'appui sur une touche de tir (°). Un missile part par appui sur la touche du groupe, du
        // lance-missiles de ce groupe qui en a tiré le moins ; la cadence est alors son temps de rechargement.
        std::int32_t ammo = 0;
        float guidance = 90.0f;
        float lockAngle = 10.0f;
        // Pendant le jeu (non enregistrés) : temps avant le prochain tir (s), missiles déjà tirés.
        float cooldown = 0.0f;
        std::int32_t fired = 0;
    };

    // Points de vie d'une cible (vaisseau, décor) : les projectiles qui touchent ses corps de collision, ou ceux de
    // ses enfants, les retirent ; à 0, elle est détruite avec ses enfants (sauf si elle porte le joueur : elle reste
    // à 0, la fin de partie viendra plus tard). Pendant le jeu, sur la copie de la scène : le fichier garde la valeur
    // de départ.
    struct HealthComponent
    {
        float health = 100.0f;
    };

    // Commandes du pilote pour les armes de son vaisseau.
    struct WeaponInput
    {
        // Groupes d'armes actifs (bit 0 : groupe 1, ... bit 9 : groupe 10 ; touches 1 à 0). Les canons d'un groupe actif
        // tirent seuls ; un lance-missiles d'un groupe actif lance un missile (voir Update). Rien ne part sans cible
        // verrouillée à la portée de l'arme.
        std::uint16_t active = 0;
        bool aim = false;  // aimPoint donné : les tourelles s'y tournent ; sinon elles reviennent vers l'avant
        Flux::Math::WorldPosition aimPoint;
        Flux::Entity target = Flux::NullEntity; // cible verrouillée (FindLockTarget) : visée en avance, poursuivie par les missiles
        // Erreur de visée ajoutée au point visé sur la cible verrouillée (m, monde) : l'imprécision d'un pilote PNJ.
        Flux::Math::Vector3f aimOffset{};
        bool missiles = true; // faux : les lance-missiles attendent (un PNJ espace ses tirs de missiles)
    };

    // Armes des vaisseaux : une arme (WeaponComponent) n'est active que posée sur un emport (HardpointComponent, son
    // parent direct) qui l'accepte (Fits), dans le vaisseau piloté.
    namespace Weapons {
        // L'emport accepte l'arme : même genre, pas plus grande que lui.
        bool Fits(const HardpointComponent& hardpoint, const WeaponComponent& weapon);
        // Pourquoi l'emport refuse l'arme (texte pour l'éditeur), vide s'il l'accepte.
        std::string GetRefusal(const HardpointComponent& hardpoint, const WeaponComponent& weapon);

        // Vaisseau piloté (NullEntity : aucun) : ses armes pivotent vers le point visé et tirent. Les projectiles
        // tirés sont ajoutés à `projectiles` (Engine::GetProjectiles). Les armes des autres vaisseaux ne bougent pas.
        // Après la physique (le vaisseau est à sa place de cette image). Au plus un missile part par image : retourne
        // le groupe (bit) qui l'a lancé, 0 sinon, pour que l'appelant désactive ce groupe (un appui, un missile).
        std::uint16_t Update(Flux::Scene& scene, Flux::Entity ship, double deltaSeconds, const WeaponInput& input,
                             std::vector<Flux::Projectile>& projectiles);

        // Cible à verrouiller quand le pilote appuie sur une touche de tir : celle qui a des points de vie, à portée des
        // armes du vaisseau, la plus proche de la direction regardée depuis `from` sans s'en écarter de plus de leur
        // angle de verrouillage. NullEntity sans arme montée, ou si rien n'est dans le viseur. Ni le vaisseau, ni ce
        // qui le porte.
        Flux::Entity FindLockTarget(const Flux::Scene& scene, Flux::Entity ship, const Flux::Math::WorldPosition& from,
                              const Flux::Math::Vector3f& direction);
        // Le verrou tient tant que la cible existe, a des points de vie et reste à portée des armes du vaisseau (elle
        // peut sortir du viseur).
        bool KeepsLock(const Flux::Scene& scene, Flux::Entity ship, Flux::Entity target, const Flux::Math::WorldPosition& from);
        // Missiles restants dans les lance-missiles du vaisseau ; -1 s'ils sont illimités, 0 sans lance-missiles.
        int GetMissileCount(const Flux::Scene& scene, Flux::Entity ship);

        // Cible d'un corps touché : l'entité elle-même ou son ancêtre le plus proche qui a des points de vie.
        Flux::Entity FindTarget(const Flux::Scene& scene, Flux::Entity entity);
        // Retire les dégâts des impacts aux cibles. Celles qui tombent à 0 sont détruites avec leurs enfants, sauf
        // si elles portent `protectedEntity` (le joueur) ; leurs noms sont ajoutés à `destroyed` s'il est donné.
        void ApplyHits(Flux::Scene& scene, std::span<const Flux::ProjectileHit> hits, Flux::Entity protectedEntity,
                       std::vector<std::string>* destroyed = nullptr);
    }

    // Emports, armes et points de vie dans l'inspecteur et les scènes (mêmes clés JSON qu'au temps du moteur).
    void RegisterWeaponComponents(Flux::ComponentRegistry& registry);
    // Aimant de l'éditeur : une arme lâchée sur un emport, ou près de lui, s'y fixe s'il l'accepte.
    void RegisterWeaponMount(Flux::EditorExtensions& extensions);
    // Dégâts des projectiles arrivés de l'image (Engine::GetProjectileHits), avant les tirs du joueur et des ennemis.
    void RegisterDamage(Flux::Engine& engine);
    void UnregisterDamage(Flux::Engine& engine);
}
