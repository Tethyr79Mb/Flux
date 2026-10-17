#include "Flux/Scene/CombatAI.hpp"

#include "Flux/Scene/Scene.hpp"
#include "Steering.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Flux::CombatAI {
    namespace {
        constexpr float BreakTime = 2.5f;       // s à filer de côté avant de revenir à l'attaque
        constexpr float FireAngle = 15.0f;      // ° entre son nez et la cible : il tire
        constexpr float PatrolSpeed = 0.3f;     // part de ShipComponent::maxSpeed en patrouille
        constexpr float LoseFactor = 1.5f;      // cible perdue au-delà de detectionRange fois autant
        constexpr float MissileInterval = 6.0f; // s entre deux missiles d'un PNJ prudent (moitié pour un kamikaze)
        constexpr float LeadSpeed = 1500.0f;    // m/s : vitesse supposée des obus pour viser en avance

        bool CanFly(const Scene& scene, Entity entity, const ShipComponent& ship) {
            return scene.GetParent(entity) == NullEntity && ship.warp == WarpStage::Off && ship.autopilot == AutopilotStage::Off
                   && ship.maxSpeed > 0.0f;
        }

        bool IsTarget(const Scene& scene, Entity self, Entity target) {
            return target != NullEntity && target != self && scene.IsValid(target);
        }

        Math::Vector3f VelocityOf(const Scene& scene, Entity entity) {
            const auto* ship = scene.TryGet<ShipComponent>(entity);
            return ship ? scene.GetWorldRotation(entity).Rotate(ship->velocity) : Math::Vector3f{};
        }

        // Côté où rompre : perpendiculaire à la cible, d'un côté ou de l'autre selon l'entité (deux ennemis ne
        // rompent pas du même côté), un peu vers le haut.
        Math::Vector3f BreakAway(Entity entity, const Math::Vector3f& toTarget, const Math::Vector3f& up) {
            Math::Vector3f side = Math::Cross(toTarget, up);
            if (Math::Length(side) < 1.0e-3f) {
                side = Math::Cross(toTarget, Math::Vector3f{1.0f, 0.0f, 0.0f});
            }
            side = Math::Normalize(side) * ((static_cast<std::uint32_t>(entity) & 1u) != 0u ? 1.0f : -1.0f);
            return Math::Normalize(side + up * 0.4f - toTarget * 0.3f);
        }

        void Enter(CombatAIComponent& ai, CombatState state) {
            ai.state = state;
            ai.stateTime = 0.0f;
        }

        // Vole vers direction (monde, unitaire) à speed : le nez s'y tourne, la vitesse suit le nez (comme en vol
        // normal), à l'accélération du vaisseau.
        void Fly(ShipComponent& ship, const Math::Quaternionf& rotation, const Math::Vector3f& direction, float speed, float delta) {
            const Math::Vector3f up = rotation.Rotate(Math::Vector3f{0.0f, 1.0f, 0.0f});
            Steering::Steer(ship, rotation, Steering::Look(direction, up), delta);
            const Math::Vector3f forward = rotation.Rotate(Math::Vector3f{0.0f, 0.0f, -1.0f});
            // Tourné à l'envers, il ralentit en virant (pas en marche arrière à pleine vitesse).
            const float facing = std::max(Math::Dot(forward, direction), 0.3f);
            const Math::Vector3f wanted = forward * (speed * facing);
            const Math::Vector3f current = rotation.Rotate(ship.velocity);
            Math::Vector3f change = wanted - current;
            const float step = 2.0f * std::max(ship.acceleration, 0.1f) * std::max(ship.boost, 1.0f) * delta;
            if (const float length = Math::Length(change); length > step) {
                change = change * (step / length);
            }
            ship.velocity = rotation.Conjugate().Rotate(current + change);
            ship.cruiseSpeed = std::max(-ship.velocity.z, 0.0f);
            ship.throttle = std::clamp(speed / std::max(ship.maxSpeed, 0.1f), 0.1f, 1.0f);
        }
    }

    void Steer(Scene& scene, double deltaSeconds, Entity target) {
        const float delta = static_cast<float>(deltaSeconds);
        if (delta <= 0.0f) {
            return; // pause
        }
        for (const auto [entity, ai, ship] : scene.GetRegistry().view<CombatAIComponent, ShipComponent>().each()) {
            ai.firing = false;
            if (!CanFly(scene, entity, ship)) {
                continue;
            }
            ai.clock += delta;
            ai.stateTime += delta;
            ai.missileCooldown = std::max(ai.missileCooldown - delta, 0.0f);
            const Math::WorldPosition position = scene.GetWorldPosition(entity);
            const Math::Quaternionf rotation = scene.GetWorldRotation(entity);
            if (!ai.hasHome) {
                ai.home = position;
                ai.hasHome = true;
            }
            const auto* health = scene.TryGet<HealthComponent>(entity);
            const float current = health ? health->health : 0.0f;
            if (ai.startHealth < 0.0f) {
                ai.startHealth = current;
                ai.lastHealth = current;
            }
            const bool hit = current < ai.lastHealth;
            ai.lastHealth = current;

            const bool hasTarget = IsTarget(scene, entity, target);
            const Math::Vector3d offset = hasTarget ? Math::Delta(position, scene.GetWorldPosition(target)) : Math::Vector3d{};
            const double distance = hasTarget ? Math::Length(offset) : std::numeric_limits<double>::infinity();
            const Math::Vector3f toTarget = distance > 1.0e-3 && hasTarget ? Math::Vector3f(offset / distance) : Math::Vector3f{0.0f, 0.0f, -1.0f};
            const Math::Vector3f up = rotation.Rotate(Math::Vector3f{0.0f, 1.0f, 0.0f});
            const float aggression = std::clamp(ai.aggression, 0.0f, 1.0f);
            const double breakDistance = ai.preferredRange * (0.6 - 0.5 * aggression);

            // Transitions.
            if (ai.state != CombatState::Flee && ai.startHealth > 0.0f && current < ai.startHealth * ai.fleeHealth / 100.0f) {
                Enter(ai, CombatState::Flee);
            }
            switch (ai.state) {
            case CombatState::Patrol:
                if (hasTarget && (distance < ai.detectionRange || hit)) {
                    Enter(ai, CombatState::Attack);
                }
                break;
            case CombatState::Attack:
                if (!hasTarget || distance > ai.detectionRange * LoseFactor) {
                    Enter(ai, CombatState::Patrol);
                } else if (distance < breakDistance || (hit && aggression < 0.5f && ai.stateTime > 3.0f)) {
                    ai.breakDirection = BreakAway(entity, toTarget, up);
                    Enter(ai, CombatState::Break);
                }
                break;
            case CombatState::Break:
                if (ai.stateTime > BreakTime) {
                    Enter(ai, hasTarget ? CombatState::Attack : CombatState::Patrol);
                }
                break;
            case CombatState::Flee:
                // Loin de la cible : il patrouille là. Rattrapé, il la voit, l'attaque et refuit aussitôt.
                if (!hasTarget || distance > ai.detectionRange * 2.0f) {
                    ai.home = position;
                    Enter(ai, CombatState::Patrol);
                }
                break;
            }

            // Cap et vitesse de l'état.
            const float boosted = ship.maxSpeed * std::max(ship.boost, 1.0f);
            switch (ai.state) {
            case CombatState::Patrol: {
                // Sur un cercle autour du point de départ, dans le plan de son repère d'origine.
                const float radius = std::max(ai.patrolRadius, 1.0f);
                const float angle = ai.clock * ship.maxSpeed * PatrolSpeed / radius;
                Math::WorldPosition point = ai.home;
                point += Math::Vector3d{std::cos(angle) * radius, 0.0, std::sin(angle) * radius};
                const Math::Vector3d toPoint = Math::Delta(position, point);
                const double length = Math::Length(toPoint);
                if (length > 1.0) {
                    Fly(ship, rotation, Math::Vector3f(toPoint / length), ship.maxSpeed * PatrolSpeed, delta);
                }
                break;
            }
            case CombatState::Attack: {
                // Vers où sera la cible quand ses obus l'atteindront.
                const Math::Vector3d lead = offset + Math::Vector3d(VelocityOf(scene, target) - rotation.Rotate(ship.velocity))
                                                         * std::min(distance / LeadSpeed, 2.0);
                const Math::Vector3f direction = Math::Length(lead) > 1.0e-3 ? Math::Vector3f(Math::Normalize(lead)) : toTarget;
                const float speed = distance > ai.preferredRange * 2.0 ? boosted
                                    : distance > ai.preferredRange ? ship.maxSpeed
                                                                   : ship.maxSpeed * (0.3f + 0.4f * aggression);
                Fly(ship, rotation, direction, speed, delta);
                const Math::Vector3f forward = rotation.Rotate(Math::Vector3f{0.0f, 0.0f, -1.0f});
                ai.firing = Math::Dot(forward, toTarget) > std::cos(FireAngle / Steering::Degrees);
                break;
            }
            case CombatState::Break: Fly(ship, rotation, ai.breakDirection, boosted, delta); break;
            case CombatState::Flee: Fly(ship, rotation, toTarget * -1.0f, boosted, delta); break;
            }
        }
    }

    void Fire(Scene& scene, double deltaSeconds, Entity target, std::vector<Projectile>& projectiles) {
        if (deltaSeconds <= 0.0) {
            return;
        }
        auto view = scene.GetRegistry().view<CombatAIComponent, ShipComponent>();
        for (const Entity entity : view) {
            CombatAIComponent& ai = view.get<CombatAIComponent>(entity);
            const bool shooting = ai.firing && IsTarget(scene, entity, target);
            // Sans tir, les tourelles reviennent vers l'avant et les canons refroidissent.
            WeaponInput input;
            if (shooting) {
                input.active = 0xFFFF;
                input.target = target;
                input.missiles = ai.missileCooldown <= 0.0f;
                // Erreur de visée : un point qui tourne lentement autour de la cible, à `inaccuracy` degrés.
                const double distance = Math::Length(Math::Delta(scene.GetWorldPosition(entity), scene.GetWorldPosition(target)));
                const float seed = static_cast<float>(static_cast<std::uint32_t>(entity) % 97u);
                const Math::Vector3f wobble = Math::Vector3f{std::sin(ai.clock * 1.3f + seed), std::sin(ai.clock * 1.7f + seed * 2.0f),
                                                             std::sin(ai.clock * 2.1f + seed * 3.0f)}
                                              * 0.577f; // au plus 1 : l'angle donné
                input.aimOffset = wobble * static_cast<float>(distance * std::tan(std::max(ai.inaccuracy, 0.0f) / Steering::Degrees));
            }
            if (Weapons::Update(scene, entity, deltaSeconds, input, projectiles) != 0) {
                ai.missileCooldown = MissileInterval * (1.0f - 0.5f * std::clamp(ai.aggression, 0.0f, 1.0f));
            }
        }
    }
}
