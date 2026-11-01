#include "Weapons.hpp"

#include <Flux/Core/EditorExtensions.hpp>
#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Scene/ComponentRegistry.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/Ships.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <initializer_list>
#include <numbers>

namespace Game::Weapons {
    using namespace Flux;

    namespace {
        constexpr float Radians = std::numbers::pi_v<float> / 180.0f;
        constexpr Math::Vector3f Forward{0.0f, 0.0f, -1.0f};
        // Au plus de tirs par arme et par image : après une longue image (chargement, pause du débogueur), la cadence
        // rattrapée d'un coup ferait une gerbe de projectiles au même endroit.
        constexpr int MaxShotsPerFrame = 4;

        const char* KindName(WeaponKind kind) {
            return kind == WeaponKind::Gun ? "canon" : "lance-missiles";
        }

        // L'une est l'ancêtre de l'autre (ou c'est la même) : un vaisseau ne verrouille ni lui-même, ni ses pièces, ni
        // la station qui le porte.
        bool Related(const Scene& scene, Entity a, Entity b) {
            for (Entity current = a; current != NullEntity; current = scene.GetParent(current)) {
                if (current == b) {
                    return true;
                }
            }
            for (Entity current = b; current != NullEntity; current = scene.GetParent(current)) {
                if (current == a) {
                    return true;
                }
            }
            return false;
        }

        // Arme de ce vaisseau, sur un emport qui l'accepte.
        bool IsMounted(const Scene& scene, Entity entity, const WeaponComponent& weapon, Entity ship) {
            if (scene.GetShip(entity) != ship) {
                return false;
            }
            const Entity hardpointEntity = scene.GetParent(entity);
            const HardpointComponent* hardpoint = hardpointEntity != NullEntity ? scene.TryGet<HardpointComponent>(hardpointEntity) : nullptr;
            return hardpoint && Fits(*hardpoint, weapon);
        }

        // Lance-missiles en état de tirer de ce vaisseau : monté et pas vide.
        bool IsLoadedLauncher(const Scene& scene, Entity entity, const WeaponComponent& weapon, Entity ship) {
            return weapon.kind == WeaponKind::Missile && IsMounted(scene, entity, weapon, ship)
                   && (weapon.ammo <= 0 || weapon.fired < weapon.ammo);
        }

        // Le plus large verrouillage (rad) et la plus longue portée (m) des armes du vaisseau ; angle négatif sans arme.
        std::pair<float, float> GetLockReach(const Scene& scene, Entity ship) {
            float lockAngle = -1.0f;
            float range = 0.0f;
            for (const auto [entity, weapon] : scene.GetRegistry().view<const WeaponComponent>().each()) {
                if (IsMounted(scene, entity, weapon, ship)) {
                    lockAngle = std::max(lockAngle, weapon.lockAngle * Radians);
                    range = std::max(range, weapon.range);
                }
            }
            return {lockAngle, range};
        }

        // Le groupe de l'arme est dans ce masque de groupes (WeaponInput).
        bool InGroups(const WeaponComponent& weapon, std::uint16_t groups) {
            return weapon.group >= 1 && weapon.group <= 10 && (groups & (1u << (weapon.group - 1))) != 0;
        }

        // Rotation locale d'une arme dont le canon (-z) pointe vers `forward` (repère de l'emport), sans roulis.
        Math::Quaternionf Aim(const Math::Vector3f& forward) {
            const float yaw = std::atan2(-forward.x, -forward.z);
            const float pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
            return Math::Quaternionf::FromYawPitchRoll(yaw, pitch, 0.0f);
        }
    }
    bool Fits(const HardpointComponent& hardpoint, const WeaponComponent& weapon) {
        return weapon.kind == hardpoint.kind && weapon.size <= hardpoint.size;
    }

    std::string GetRefusal(const HardpointComponent& hardpoint, const WeaponComponent& weapon) {
        if (weapon.kind != hardpoint.kind) {
            return std::format("cet emport accepte un {}, pas un {}", KindName(hardpoint.kind), KindName(weapon.kind));
        }
        if (weapon.size > hardpoint.size) {
            return std::format("arme de taille {} trop grande pour cet emport (taille {} au plus)", weapon.size, hardpoint.size);
        }
        return {};
    }

    std::uint16_t Update(Scene& scene, Entity ship, double deltaSeconds, const WeaponInput& input, std::vector<Projectile>& projectiles) {
        const float delta = static_cast<float>(deltaSeconds);
        if (ship == NullEntity || !scene.IsValid(ship) || delta <= 0.0f) {
            return 0; // pause : les tourelles ne bougent pas, rien ne part
        }
        const Math::Vector3f shipVelocity = Ships::GetWorldVelocity(scene, ship);
        auto& registry = scene.GetRegistry();
        // Cible verrouillée : les armes la visent et ne tirent que si elle est à leur portée ; sans elle, rien ne part.
        const bool locked = input.target != NullEntity && scene.IsValid(input.target);
        const Math::WorldPosition targetPosition = locked ? scene.GetWorldPosition(input.target) : Math::WorldPosition{};
        const Math::Vector3f targetVelocity = locked ? Ships::GetWorldVelocity(scene, input.target) : Math::Vector3f{};
        // Lance-missiles qui tirera, d'un groupe actif : prêt, celui qui a le moins tiré (les deux
        // ailes à tour de rôle).
        Entity launcher = NullEntity;
        for (const auto [entity, weapon] : registry.view<WeaponComponent>().each()) {
            const Entity hardpointEntity = scene.GetParent(entity);
            if (hardpointEntity == NullEntity || scene.GetShip(entity) != ship) {
                continue;
            }
            const HardpointComponent* hardpoint = scene.TryGet<HardpointComponent>(hardpointEntity);
            if (!hardpoint || !Fits(*hardpoint, weapon)) {
                continue;
            }

            // Visée : vers la cible verrouillée, là où elle sera quand les obus l'atteindront (vitesse relative au
            // vaisseau, qui les emporte) ; sinon vers le point regardé. Ramenée dans le cône de l'emport, à la vitesse
            // de pivotement.
            TransformComponent& transform = scene.Get<TransformComponent>(entity);
            const Math::Vector3f current = Math::Normalize(transform.rotation.Rotate(Forward));
            Math::Vector3f wanted = Forward;
            const Math::WorldPosition weaponPosition = scene.GetWorldPosition(entity);
            const double targetDistance = locked ? Math::Length(Math::Delta(weaponPosition, targetPosition)) : 0.0;
            if (locked || input.aim) {
                Math::Vector3d toTarget = Math::Delta(weaponPosition, locked ? targetPosition : input.aimPoint);
                if (locked && weapon.kind == WeaponKind::Gun && weapon.projectileSpeed > 0.0f) {
                    toTarget = toTarget + Math::Vector3d(targetVelocity - shipVelocity) * (targetDistance / weapon.projectileSpeed);
                }
                if (locked) {
                    toTarget = toTarget + Math::Vector3d(input.aimOffset);
                }
                if (Math::Length(toTarget) > 1.0e-3) {
                    const Math::Vector3f local =
                        scene.GetWorldRotation(hardpointEntity).Conjugate().Rotate(Math::Vector3f(Math::Normalize(toTarget)));
                    wanted = Math::TurnToward(Forward, local, std::max(weapon.cone, 0.0f) * Radians);
                }
            }
            const Math::Vector3f aimed = Math::TurnToward(current, wanted, std::max(weapon.turnRate, 0.0f) * Radians * delta);
            transform.rotation = Aim(aimed);

            weapon.cooldown = std::max(weapon.cooldown - delta, 0.0f);
            const bool inRange = locked && targetDistance <= weapon.range;
            if (weapon.kind == WeaponKind::Missile) {
                if (inRange && input.missiles && InGroups(weapon, input.active) && weapon.cooldown <= 0.0f && weapon.projectileSpeed > 0.0f && IsLoadedLauncher(scene, entity, weapon, ship)
                    && (launcher == NullEntity || weapon.fired < registry.get<WeaponComponent>(launcher).fired)) {
                    launcher = entity;
                }
                continue;
            }
            if (!inRange || !InGroups(weapon, input.active) || weapon.kind != WeaponKind::Gun || weapon.fireRate <= 0.0f || weapon.projectileSpeed <= 0.0f) {
                continue;
            }
            const Math::Quaternionf rotation = scene.GetWorldRotation(entity);
            const Math::Vector3f direction = Math::Normalize(rotation.Rotate(Forward));
            const Math::WorldPosition origin = scene.GetWorldPosition(entity);
            const float interval = 1.0f / weapon.fireRate;
            for (int shot = 0; shot < MaxShotsPerFrame && weapon.cooldown <= 0.0f; ++shot) {
                Projectile projectile;
                projectile.position = origin;
                projectile.position += Math::Vector3d(direction * std::max(weapon.muzzle, 0.0f));
                projectile.velocity = shipVelocity + direction * weapon.projectileSpeed;
                projectile.life = std::max(weapon.range, 0.0f) / weapon.projectileSpeed;
                projectile.damage = weapon.damage;
                projectile.color = weapon.color;
                projectile.ship = ship;
                projectiles.push_back(projectile);
                weapon.cooldown += interval;
            }
            weapon.cooldown = std::max(weapon.cooldown, 0.0f);
        }

        if (launcher != NullEntity) {
            WeaponComponent& weapon = registry.get<WeaponComponent>(launcher);
            const Math::Vector3f direction = Math::Normalize(scene.GetWorldRotation(launcher).Rotate(Forward));
            Projectile missile;
            missile.position = scene.GetWorldPosition(launcher);
            missile.position += Math::Vector3d(direction * std::max(weapon.muzzle, 0.0f));
            missile.velocity = shipVelocity + direction * weapon.projectileSpeed;
            missile.life = std::max(weapon.range, 0.0f) / weapon.projectileSpeed;
            missile.damage = weapon.damage;
            missile.color = weapon.color;
            missile.ship = ship;
            missile.missile = true;
            missile.target = input.target;
            missile.guidance = std::max(weapon.guidance, 0.0f) * Radians;
            projectiles.push_back(missile);
            weapon.cooldown = weapon.fireRate > 0.0f ? 1.0f / weapon.fireRate : 0.0f;
            ++weapon.fired;
            return static_cast<std::uint16_t>(1u << (weapon.group - 1));
        }
        return 0;
    }

    Entity FindLockTarget(const Scene& scene, Entity ship, const Math::WorldPosition& from, const Math::Vector3f& direction) {
        if (ship == NullEntity || !scene.IsValid(ship) || Math::Length(direction) < 1.0e-6f) {
            return NullEntity;
        }
        auto& registry = scene.GetRegistry();
        const auto [lockAngle, range] = GetLockReach(scene, ship);
        if (lockAngle < 0.0f) {
            return NullEntity;
        }
        const Math::Vector3d forward(Math::Normalize(direction));
        const double cosLock = std::cos(static_cast<double>(lockAngle));
        Entity best = NullEntity;
        double bestCos = -2.0;
        for (const auto [entity, health] : registry.view<const HealthComponent>().each()) {
            if (health.health <= 0.0f) {
                continue;
            }
            const Math::Vector3d toTarget = Math::Delta(from, scene.GetWorldPosition(entity));
            const double distance = Math::Length(toTarget);
            if (distance < 1.0e-3 || distance > range) {
                continue;
            }
            const double cosine = Math::Dot(toTarget, forward) / distance;
            if (cosine < cosLock || cosine <= bestCos || Related(scene, entity, ship)) {
                continue;
            }
            best = entity;
            bestCos = cosine;
        }
        return best;
    }

    bool KeepsLock(const Scene& scene, Entity ship, Entity target, const Math::WorldPosition& from) {
        if (ship == NullEntity || target == NullEntity || !scene.IsValid(ship) || !scene.IsValid(target)) {
            return false;
        }
        const HealthComponent* health = scene.TryGet<HealthComponent>(target);
        const auto [lockAngle, range] = GetLockReach(scene, ship);
        return health && health->health > 0.0f && lockAngle >= 0.0f
               && Math::Length(Math::Delta(from, scene.GetWorldPosition(target))) <= range;
    }

    int GetMissileCount(const Scene& scene, Entity ship) {
        if (ship == NullEntity || !scene.IsValid(ship)) {
            return 0;
        }
        int count = 0;
        for (const auto [entity, weapon] : scene.GetRegistry().view<const WeaponComponent>().each()) {
            if (weapon.kind != WeaponKind::Missile || scene.GetShip(entity) != ship) {
                continue;
            }
            const Entity hardpointEntity = scene.GetParent(entity);
            const HardpointComponent* hardpoint = hardpointEntity != NullEntity ? scene.TryGet<HardpointComponent>(hardpointEntity) : nullptr;
            if (!hardpoint || !Fits(*hardpoint, weapon)) {
                continue;
            }
            if (weapon.ammo <= 0) {
                return -1;
            }
            count += std::max(weapon.ammo - weapon.fired, 0);
        }
        return count;
    }

    Entity FindTarget(const Scene& scene, Entity entity) {
        for (Entity current = entity; current != NullEntity && scene.IsValid(current); current = scene.GetParent(current)) {
            if (scene.TryGet<HealthComponent>(current)) {
                return current;
            }
        }
        return NullEntity;
    }

    void ApplyHits(Scene& scene, std::span<const ProjectileHit> hits, Entity protectedEntity, std::vector<std::string>* destroyed) {
        for (const ProjectileHit& hit : hits) {
            const Entity target = FindTarget(scene, hit.entity);
            if (target == NullEntity) {
                continue; // décor sans points de vie : l'impact se voit, rien ne casse
            }
            HealthComponent& health = scene.Get<HealthComponent>(target);
            if (health.health <= 0.0f) {
                continue; // déjà à 0 (porte le joueur)
            }
            health.health = std::max(health.health - std::max(hit.damage, 0.0f), 0.0f);
            if (health.health > 0.0f) {
                continue;
            }
            bool carriesProtected = false;
            for (Entity current = protectedEntity; current != NullEntity && scene.IsValid(current); current = scene.GetParent(current)) {
                carriesProtected |= current == target;
            }
            if (carriesProtected) {
                continue;
            }
            if (destroyed) {
                destroyed->push_back(scene.Get<NameComponent>(target).name);
            }
            scene.DestroyEntity(target); // les impacts suivants sur elle ne trouvent plus d'entité valide
        }
    }

}

namespace Game {
    using namespace Flux;

    namespace {
        constexpr const char* DamageSystem = "Armes : dégâts";
        std::vector<std::string> g_destroyed; // noms de l'image, gardés d'une image à l'autre : pas d'allocation

        void Damage(Engine& engine, double) {
            g_destroyed.clear();
            Weapons::ApplyHits(engine.GetScene(), engine.GetProjectileHits(), engine.GetPlayer(), &g_destroyed);
            for (const std::string& name : g_destroyed) {
                Log::Info("« {} » détruit", name);
            }
        }
    }

    void RegisterWeaponComponents(ComponentRegistry& registry) {
        const std::initializer_list<EnumValue<WeaponKind>> weaponKinds{{"gun", "Canon", WeaponKind::Gun},
                                                                       {"missile", "Missiles", WeaponKind::Missile}};
        registry.Register<HardpointComponent>("hardpoint", "Emport")
            .Enum("kind", "Accepte", &HardpointComponent::kind, weaponKinds)
            .Field("size", "Taille max (1 à 3)", &HardpointComponent::size, {.speed = 0.05, .min = 1.0, .max = 3.0, .decimals = 0});
        registry.Register<WeaponComponent>("weapon", "Arme")
            .Enum("kind", "Genre", &WeaponComponent::kind, weaponKinds)
            .Field("size", "Taille (1 à 3)", &WeaponComponent::size, {.speed = 0.05, .min = 1.0, .max = 3.0, .decimals = 0})
            .Field("group", "Groupe de tir (touches 1 à 9 ; 10 : touche 0)", &WeaponComponent::group, {.speed = 0.05, .min = 0.0, .max = 10.0, .decimals = 0})
            .Field("fireRate", "Cadence (tirs/s)", &WeaponComponent::fireRate, {.speed = 0.05, .min = 0.1, .max = 100.0, .decimals = 2})
            .Field("projectileSpeed", "Vitesse des projectiles (m/s)", &WeaponComponent::projectileSpeed,
                   {.speed = 5.0, .min = 10.0, .max = 1.0e5, .decimals = 0})
            .Field("range", "Portée (m)", &WeaponComponent::range, {.speed = 5.0, .min = 10.0, .max = 1.0e6, .decimals = 0})
            .Field("damage", "Dégâts", &WeaponComponent::damage, {.speed = 0.1, .min = 0.0, .max = 1.0e6, .decimals = 1})
            .Field("cone", "Débattement (°)", &WeaponComponent::cone, {.speed = 0.1, .min = 0.0, .max = 180.0, .decimals = 1})
            .Field("turnRate", "Pivotement (°/s)", &WeaponComponent::turnRate, {.speed = 1.0, .min = 0.0, .max = 3600.0, .decimals = 0})
            .Field("muzzle", "Bouche du canon (m)", &WeaponComponent::muzzle, {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("color", "Couleur des traçantes", &WeaponComponent::color, {.alpha = false})
            .Field("ammo", "Missiles emportés (0 : illimités)", &WeaponComponent::ammo, {.speed = 0.05, .min = 0.0, .max = 1000.0, .decimals = 0})
            .Field("guidance", "Missiles : virage (°/s)", &WeaponComponent::guidance, {.speed = 1.0, .min = 0.0, .max = 3600.0, .decimals = 0})
            .Field("lockAngle", "Verrouillage (°)", &WeaponComponent::lockAngle, {.speed = 0.1, .min = 0.0, .max = 90.0, .decimals = 1});
        registry.Register<HealthComponent>("health", "Points de vie")
            .Field("health", "Points de vie", &HealthComponent::health, {.speed = 1.0, .min = 0.0, .max = 1.0e9, .decimals = 0});
    }

    void RegisterWeaponMount(EditorExtensions& extensions) {
        extensions.AddMountRule(MountRule{
            .title = "Emport",
            .isPart = [](const Scene& scene, Entity entity) { return scene.TryGet<WeaponComponent>(entity) != nullptr; },
            .isMount = [](const Scene& scene, Entity entity) { return scene.TryGet<HardpointComponent>(entity) != nullptr; },
            .refusal = [](const Scene& scene, Entity weapon, Entity hardpoint) -> std::string {
                const auto* component = scene.TryGet<WeaponComponent>(weapon);
                const auto* mount = scene.TryGet<HardpointComponent>(hardpoint);
                if (!component || !mount) {
                    return {};
                }
                if (std::string refusal = Weapons::GetRefusal(*mount, *component); !refusal.empty()) {
                    return refusal;
                }
                for (const Entity other : scene.GetRegistry().view<const WeaponComponent>()) {
                    if (other != weapon && scene.GetParent(other) == hardpoint) {
                        return "cet emport porte déjà une arme (« " + scene.Get<NameComponent>(other).name + " »)";
                    }
                }
                return {};
            },
        });
    }

    void RegisterDamage(Engine& engine) { engine.AddGameSystem(Engine::GameStage::AfterPhysics, DamageSystem, Damage); }

    void UnregisterDamage(Engine& engine) { engine.RemoveGameSystem(DamageSystem); }
}
