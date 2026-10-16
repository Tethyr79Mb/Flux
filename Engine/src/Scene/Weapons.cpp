#include "Flux/Scene/Weapons.hpp"

#include "Flux/Scene/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace Flux::Weapons {
    namespace {
        constexpr float Radians = std::numbers::pi_v<float> / 180.0f;
        constexpr Math::Vector3f Forward{0.0f, 0.0f, -1.0f};
        // Au plus de tirs par arme et par image : après une longue image (chargement, pause du débogueur), la cadence
        // rattrapée d'un coup ferait une gerbe de projectiles au même endroit.
        constexpr int MaxShotsPerFrame = 4;

        const char* KindName(WeaponKind kind) {
            return kind == WeaponKind::Gun ? "canon" : "lance-missiles";
        }

        // Tourne `from` vers `to` (unitaires) d'au plus `angle` radians.
        Math::Vector3f TurnToward(const Math::Vector3f& from, const Math::Vector3f& to, float angle) {
            const float between = std::acos(std::clamp(Math::Dot(from, to), -1.0f, 1.0f));
            if (between <= angle) {
                return to;
            }
            Math::Vector3f axis = Math::Cross(from, to);
            if (Math::Length(axis) < 1.0e-6f) {
                axis = std::abs(from.y) < 0.9f ? Math::Vector3f{0.0f, 1.0f, 0.0f} : Math::Vector3f{1.0f, 0.0f, 0.0f};
            }
            return Math::Normalize(Math::Quaternionf::FromAxisAngle(axis, angle).Rotate(from));
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

        // Lance-missiles en état de tirer de ce vaisseau : sur un emport qui l'accepte et pas vide.
        bool IsLoadedLauncher(const Scene& scene, Entity entity, const WeaponComponent& weapon, Entity ship) {
            if (weapon.kind != WeaponKind::Missile || scene.GetShip(entity) != ship) {
                return false;
            }
            const Entity hardpointEntity = scene.GetParent(entity);
            const HardpointComponent* hardpoint = hardpointEntity != NullEntity ? scene.TryGet<HardpointComponent>(hardpointEntity) : nullptr;
            return hardpoint && Fits(*hardpoint, weapon) && (weapon.ammo <= 0 || weapon.fired < weapon.ammo);
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

    void Update(Scene& scene, Entity ship, double deltaSeconds, const WeaponInput& input, std::vector<Projectile>& projectiles) {
        const float delta = static_cast<float>(deltaSeconds);
        if (ship == NullEntity || !scene.IsValid(ship) || delta <= 0.0f) {
            return; // pause : les tourelles ne bougent pas, rien ne part
        }
        const ShipComponent* shipComponent = scene.TryGet<ShipComponent>(ship);
        const Math::Vector3f shipVelocity =
            shipComponent ? scene.GetWorldRotation(ship).Rotate(shipComponent->velocity) : Math::Vector3f{};
        auto& registry = scene.GetRegistry();
        // Lance-missiles qui tirera si « Tirer un missile » est appuyée : prêt, celui qui a le moins tiré (les deux
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

            // Visée : vers le point regardé, ramené dans le cône de l'emport, à la vitesse de pivotement.
            TransformComponent& transform = scene.Get<TransformComponent>(entity);
            const Math::Vector3f current = Math::Normalize(transform.rotation.Rotate(Forward));
            Math::Vector3f wanted = Forward;
            if (input.aim) {
                const Math::Vector3d toTarget = Math::Delta(scene.GetWorldPosition(entity), input.aimPoint);
                if (Math::Length(toTarget) > 1.0e-3) {
                    const Math::Vector3f local =
                        scene.GetWorldRotation(hardpointEntity).Conjugate().Rotate(Math::Vector3f(Math::Normalize(toTarget)));
                    wanted = TurnToward(Forward, local, std::max(weapon.cone, 0.0f) * Radians);
                }
            }
            const Math::Vector3f aimed = TurnToward(current, wanted, std::max(weapon.turnRate, 0.0f) * Radians * delta);
            transform.rotation = Aim(aimed);

            weapon.cooldown = std::max(weapon.cooldown - delta, 0.0f);
            if (weapon.kind == WeaponKind::Missile) {
                if (input.launch && weapon.cooldown <= 0.0f && weapon.projectileSpeed > 0.0f && IsLoadedLauncher(scene, entity, weapon, ship)
                    && (launcher == NullEntity || weapon.fired < registry.get<WeaponComponent>(launcher).fired)) {
                    launcher = entity;
                }
                continue;
            }
            if (!input.fire || weapon.kind != WeaponKind::Gun || weapon.fireRate <= 0.0f || weapon.projectileSpeed <= 0.0f) {
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
            missile.target = input.target != NullEntity && scene.IsValid(input.target) ? input.target : NullEntity;
            missile.guidance = std::max(weapon.guidance, 0.0f) * Radians;
            projectiles.push_back(missile);
            weapon.cooldown = weapon.fireRate > 0.0f ? 1.0f / weapon.fireRate : 0.0f;
            ++weapon.fired;
        }
    }

    Entity FindLockTarget(const Scene& scene, Entity ship, const Math::WorldPosition& from, const Math::Vector3f& direction) {
        if (ship == NullEntity || !scene.IsValid(ship) || Math::Length(direction) < 1.0e-6f) {
            return NullEntity;
        }
        // Le plus large verrouillage et la plus longue portée des lance-missiles chargés.
        auto& registry = scene.GetRegistry();
        float lockAngle = -1.0f;
        float range = 0.0f;
        for (const auto [entity, weapon] : registry.view<const WeaponComponent>().each()) {
            if (IsLoadedLauncher(scene, entity, weapon, ship)) {
                lockAngle = std::max(lockAngle, weapon.lockAngle * Radians);
                range = std::max(range, weapon.range);
            }
        }
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

    void GuideMissiles(const Scene& scene, std::vector<Projectile>& projectiles, double deltaSeconds) {
        const float delta = static_cast<float>(deltaSeconds);
        if (delta <= 0.0f) {
            return;
        }
        for (Projectile& projectile : projectiles) {
            if (!projectile.missile || projectile.target == NullEntity) {
                continue;
            }
            if (!scene.IsValid(projectile.target)) {
                projectile.target = NullEntity; // détruite (par un autre tir) : tout droit
                continue;
            }
            const float speed = Math::Length(projectile.velocity);
            if (speed <= 0.0f) {
                continue;
            }
            // Visée en avance sur un vaisseau qui vole : là où il sera au bout du temps de vol restant. Les autres
            // cibles ne bougent pas seules (une cible portée par un vaisseau : celle du vaisseau).
            Math::Vector3f toTarget(Math::Delta(projectile.position, scene.GetWorldPosition(projectile.target)));
            const Entity targetShip =
                scene.TryGet<ShipComponent>(projectile.target) ? projectile.target : scene.GetShip(projectile.target);
            if (const ShipComponent* ship = targetShip != NullEntity ? scene.TryGet<ShipComponent>(targetShip) : nullptr) {
                const Math::Vector3f velocity = scene.GetWorldRotation(targetShip).Rotate(ship->velocity);
                toTarget += velocity * (Math::Length(toTarget) / speed);
            }
            if (Math::Length(toTarget) < 1.0e-3f) {
                continue;
            }
            const Math::Vector3f direction = TurnToward(projectile.velocity * (1.0f / speed), Math::Normalize(toTarget), projectile.guidance * delta);
            projectile.velocity = direction * speed;
        }
    }

    void UpdateProjectiles(std::vector<Projectile>& projectiles, double deltaSeconds, const ProjectileCast& cast,
                           std::vector<ProjectileHit>* hits) {
        const float delta = static_cast<float>(deltaSeconds);
        if (delta <= 0.0f) {
            return;
        }
        for (std::size_t i = 0; i < projectiles.size();) {
            Projectile& projectile = projectiles[i];
            // Jamais plus loin que la fin de sa course.
            const float step = std::min(delta, std::max(projectile.life, 0.0f));
            const Math::Vector3f segment = projectile.velocity * step;
            bool stopped = projectile.life <= 0.0f;
            if (!stopped && cast) {
                if (const auto hit = cast(projectile.position, segment, projectile.ship)) {
                    if (hits) {
                        Math::WorldPosition at = projectile.position;
                        at += Math::Vector3d(segment * hit->second);
                        hits->push_back({hit->first, at, projectile.damage, projectile.color, projectile.missile});
                    }
                    stopped = true;
                }
            }
            projectile.life -= delta;
            if (stopped || projectile.life <= 0.0f) {
                projectile = projectiles.back();
                projectiles.pop_back();
                continue;
            }
            projectile.age += delta;
            projectile.position += Math::Vector3d(segment);
            ++i;
        }
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

    void UpdateImpacts(std::vector<Impact>& impacts, double deltaSeconds) {
        const float delta = static_cast<float>(deltaSeconds);
        std::erase_if(impacts, [delta](Impact& impact) {
            impact.life -= delta;
            return impact.life <= 0.0f;
        });
    }
}
