#include "Flux/Scene/Orbits.hpp"

#include "Flux/Scene/Components.hpp"
#include "Flux/Scene/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Flux::Orbits {
    namespace {
        constexpr double MetersPerUnit = 1.0e9; // OrbitComponent::distance : en millions de km
        constexpr double RadiansPerDegree = std::numbers::pi / 180.0;
    }

    void Update(Scene& scene) {
        entt::registry& registry = scene.GetRegistry();
        Entity sun = NullEntity;
        for (const Entity entity : registry.view<const SunComponent>()) {
            sun = entity;
            break;
        }
        if (sun == NullEntity) {
            return;
        }
        const Math::WorldPosition center = scene.GetWorldPosition(sun);
        for (const auto [entity, orbit, transform] : registry.view<OrbitComponent, TransformComponent>().each()) {
            // Un enfant suit son parent ; le soleil ne tourne pas autour de lui-même.
            if (entity == sun || scene.GetParent(entity) != NullEntity) {
                continue;
            }
            if (orbit.distance <= 0.0) {
                const Math::Vector3d offset = Math::Delta(center, scene.GetWorldPosition(entity));
                const double length = Math::Length(offset);
                if (length < 1.0) {
                    continue; // sur le soleil : pas de direction
                }
                orbit.distance = length / MetersPerUnit;
                orbit.angle = std::atan2(-offset.z, offset.x) / RadiansPerDegree;
                orbit.inclination = std::asin(std::clamp(offset.y / length, -1.0, 1.0)) / RadiansPerDegree;
            }
            // Angle 0 : vers +x ; il tourne vers -z (vers la gauche vu d'au-dessus). Inclinaison : vers +y.
            const double angle = orbit.angle * RadiansPerDegree;
            const double inclination = orbit.inclination * RadiansPerDegree;
            const Math::Vector3d direction{std::cos(inclination) * std::cos(angle), std::sin(inclination),
                                           -std::cos(inclination) * std::sin(angle)};
            Math::WorldPosition position = center;
            position += direction * (orbit.distance * MetersPerUnit);
            transform.position = position.offset;
            if (position.sector != Math::Vector3i64{}) {
                registry.emplace_or_replace<SectorComponent>(entity, position.sector);
            } else {
                registry.remove<SectorComponent>(entity);
            }
        }
    }
}
