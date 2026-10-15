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

            // Tir : les canons seulement à cette étape (missiles guidés plus tard).
            weapon.cooldown = std::max(weapon.cooldown - delta, 0.0f);
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
    }

    void UpdateProjectiles(std::vector<Projectile>& projectiles, double deltaSeconds) {
        const float delta = static_cast<float>(deltaSeconds);
        if (delta <= 0.0f) {
            return;
        }
        for (std::size_t i = 0; i < projectiles.size();) {
            Projectile& projectile = projectiles[i];
            projectile.life -= delta;
            if (projectile.life <= 0.0f) {
                projectile = projectiles.back();
                projectiles.pop_back();
                continue;
            }
            projectile.age += delta;
            projectile.position += Math::Vector3d(projectile.velocity) * deltaSeconds;
            ++i;
        }
    }
}
