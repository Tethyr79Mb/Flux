#include "Flux/Scene/Projectiles.hpp"

#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/Ships.hpp"

#include <algorithm>
#include <cmath>

namespace Flux::Projectiles {
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
            toTarget += Ships::GetWorldVelocity(scene, projectile.target) * (Math::Length(toTarget) / speed);
            if (Math::Length(toTarget) < 1.0e-3f) {
                continue;
            }
            const Math::Vector3f direction = Math::TurnToward(projectile.velocity * (1.0f / speed), Math::Normalize(toTarget), projectile.guidance * delta);
            projectile.velocity = direction * speed;
        }
    }

    void Update(std::vector<Projectile>& projectiles, double deltaSeconds, const ProjectileCast& cast,
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

    void UpdateImpacts(std::vector<Impact>& impacts, double deltaSeconds) {
        const float delta = static_cast<float>(deltaSeconds);
        std::erase_if(impacts, [delta](Impact& impact) {
            impact.life -= delta;
            return impact.life <= 0.0f;
        });
    }
}
