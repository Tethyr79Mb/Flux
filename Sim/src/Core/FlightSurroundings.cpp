#include "Core/FlightSurroundings.hpp"

#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Planetology/Planet.hpp"
#include "Flux/Scene/Scene.hpp"

#include <algorithm>
#include <cmath>

namespace Flux {
    FlightState FlightSurroundings::Get(Scene& scene, PhysicsWorld* physics, Entity ship) {
        FlightState flight;
        // L'air des planètes ou du monde (WorldComponent), comme pour les objets qui tombent.
        if (physics) {
            flight.air = std::min(1.0f, physics->GetOutsideAir(scene, ship));
        }
        const Math::WorldPosition position = scene.GetWorldPosition(ship);
        const PlanetComponent* nearest = nullptr;
        Math::Vector3d offset{};
        Math::Quaternionf rotation;
        for (const auto [entity, planet] : scene.GetRegistry().view<const PlanetComponent>().each()) {
            if (scene.GetShip(entity) != NullEntity) {
                continue; // décor d'un vaisseau
            }
            const Math::Vector3d candidate = Math::Delta(scene.GetWorldPosition(entity), position);
            if (!nearest || Math::Length(candidate) - planet.radius < Math::Length(offset) - nearest->radius) {
                nearest = &planet;
                offset = candidate;
                rotation = scene.GetWorldRotation(entity);
            }
        }
        // Dans l'espace (au-delà de deux rayons, là où s'arrête sa gravité, voir PlanetGround) : pas d'altitude.
        if (!nearest || Math::Length(offset) < 1.0 || Math::Length(offset) > 2.0 * nearest->radius) {
            return flight;
        }
        if (!m_terrain || m_terrain->GetRadius() != nearest->radius || m_terrain->GetRelief() != nearest->relief
            || m_terrainSeed != nearest->seed || m_terrainWidth != nearest->mountainWidth) {
            m_terrain = std::make_shared<const Planetology::Terrain>(nearest->radius, nearest->relief, nearest->seed, nearest->mountainWidth);
            m_terrainSeed = nearest->seed;
            m_terrainWidth = nearest->mountainWidth;
        }
        flight.nearPlanet = true;
        flight.up = Math::Normalize(offset);
        flight.center = Math::Length(offset);
        const Math::Vector3d direction = rotation.Conjugate().Rotate(flight.up);
        double ground = m_terrain->GetHeight(direction, m_terrain->GetOctaves(m_terrain->GetMaxLevel()));
        if (nearest->ocean) {
            ground = std::max(ground, nearest->seaLevel * 0.01 * nearest->relief);
        }
        flight.altitude = Math::Length(offset) - nearest->radius - ground;
        const double height = std::max(nearest->atmosphereHeight, 100.0f);
        const double above = std::max(Math::Length(offset) - nearest->radius, 0.0);
        flight.boundary = std::max(nearest->atmosphere > 0.0f ? above - height : flight.altitude, 0.0);
        return flight;
    }
}
