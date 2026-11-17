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
        const Math::WorldPosition center = sun != NullEntity ? scene.GetWorldPosition(sun) : Math::WorldPosition{};
        for (const auto [entity, orbit, transform] : registry.view<OrbitComponent, TransformComponent>().each()) {
            if (entity == sun) {
                continue; // le soleil ne tourne pas autour de lui-même
            }
            // Un enfant (une lune) tourne autour de l'origine de son parent, dans son repère : l'orbite suit le parent et
            // son équateur s'il est tourné. Sa position reste locale, sans secteur (moins d'un secteur de 1e9 m suffit).
            const bool child = scene.GetParent(entity) != NullEntity;
            if (!child && sun == NullEntity) {
                continue; // sans Soleil, une racine reste où elle est
            }
            if (orbit.distance <= 0.0) {
                const Math::Vector3d offset = child ? transform.position : Math::Delta(center, scene.GetWorldPosition(entity));
                const double length = Math::Length(offset);
                if (length < 1.0) {
                    continue; // sur son centre : pas de direction
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
            if (child) {
                transform.position = direction * (orbit.distance * MetersPerUnit);
                continue;
            }
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

    void Reparented(Scene& scene, Entity entity) {
        OrbitComponent* orbit = scene.TryGet<OrbitComponent>(entity);
        if (!orbit) {
            return;
        }
        const Entity parent = scene.GetParent(entity);
        const PlanetComponent* planet = parent != NullEntity ? scene.TryGet<PlanetComponent>(parent) : nullptr;
        if (planet && scene.TryGet<PlanetComponent>(entity)) {
            orbit->distance = MoonDistance * static_cast<double>(planet->radius) / MetersPerUnit;
            orbit->inclination = 0.0;
        } else {
            orbit->distance = 0.0;
        }
        if (parent != NullEntity) {
            scene.GetRegistry().remove<SectorComponent>(entity); // un enfant n'a que sa position locale
        }
        Update(scene);
    }
}
