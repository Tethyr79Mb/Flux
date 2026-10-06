#pragma once

// Interne à la physique : le sol des planètes (PlanetComponent) dans le monde de la scène, et leur gravité.

#include "Flux/Graphics/Planet.hpp"
#include "Flux/Math/Quaternion.hpp"
#include "Flux/Math/WorldPosition.hpp"
#include "Flux/Scene/Components.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace JPH {
    class PhysicsSystem;
}

namespace Flux {
    class Scene;

    // Sol des planètes : seulement autour de ce qui peut le toucher (personnages, vaisseaux, objets simulés), en tuiles
    // du niveau le plus fin, celles que la caméra voit de près (Planet::BuildTileSurface), en maillages Jolt immobiles.
    // Les tuiles sont faites sur un fil à part, sans arrêter le jeu ; celle juste sous un objet tout près du sol est
    // faite tout de suite s'il n'y en a pas encore (apparition, téléportation), pour qu'il ne passe jamais au travers.
    class PlanetGround {
        public:
        // Ce qui peut toucher le sol : sa position (relative à l'ancre du monde) et la distance où il lui faut du sol
        // (sa taille, plus le chemin qu'il peut faire avant que les tuiles suivantes soient prêtes).
        struct Probe
        {
            Math::Vector3d position;
            double reach = 0.0;
            double speed = 0.0; // m/s
        };

        explicit PlanetGround(JPH::ObjectLayer layer);
        ~PlanetGround();
        PlanetGround(const PlanetGround&) = delete;
        PlanetGround& operator=(const PlanetGround&) = delete;

        // Planètes de la scène (hors vaisseaux), tuiles finies ajoutées, tuiles demandées autour des sondes, tuiles
        // plus utilisées retirées. wait : toutes les tuiles demandées sont faites tout de suite (chargement de la scène).
        void Update(const Scene& scene, const Math::WorldPosition& anchor, std::span<const Probe> probes, JPH::PhysicsSystem& system,
                    bool wait);
        // L'ancre du monde a bougé : tout se décale de `shift`.
        void Shift(JPH::PhysicsSystem& system, const Math::Vector3d& shift);
        // Retire tous les corps (fin du monde).
        void Clear(JPH::PhysicsSystem& system);

        // Gravité en ce point (relatif à l'ancre) : vers le centre de la planète la plus attirante à moins de deux
        // rayons ; `fallback` (celle du monde) loin de toute planète.
        Math::Vector3f GetGravity(const Math::Vector3d& position, const Math::Vector3f& fallback) const;
        bool HasPlanets() const { return !m_planets.empty(); }
        // Tuiles de sol dans le monde (pour les tests).
        int GetTileCount() const;

        private:
        struct Tile
        {
            JPH::BodyID body; // invalide : demandée, pas encore faite
            std::uint64_t lastUsed = 0;
        };
        struct PlanetState
        {
            std::shared_ptr<const Planet::Terrain> terrain;
            std::uint64_t owner = 0;
            float radius = 0.0f;
            float relief = -1.0f;
            std::int32_t seed = 0;
            float gravity = 0.0f;
            Math::Vector3d position; // centre, relatif à l'ancre
            Math::Quaternionf rotation;
            std::unordered_map<std::uint64_t, Tile> tiles;
            bool seen = false;
        };
        struct Worker;

        void RemoveTiles(JPH::PhysicsSystem& system, PlanetState& state);
        void AddTile(JPH::PhysicsSystem& system, Entity entity, PlanetState& state, Planet::TileKey key, const Math::Vector3d& center,
                     const JPH::Shape& shape);
        void Request(const PlanetState& state, const Probe& probe, std::vector<Planet::TileKey>& keys, bool& underMissing,
                     Planet::TileKey& under) const;

        JPH::ObjectLayer m_layer;
        std::unordered_map<Entity, PlanetState> m_planets;
        std::unique_ptr<Worker> m_worker;
        std::uint64_t m_update = 0;
        std::uint64_t m_nextOwner = 1;
    };
}
