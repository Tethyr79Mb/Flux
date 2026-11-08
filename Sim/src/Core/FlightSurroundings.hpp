#pragma once

#include "Flux/Scene/Components.hpp"
#include "Flux/Scene/Ships.hpp"

#include <cstdint>
#include <memory>

namespace Flux {
    class PhysicsWorld;
    class Scene;

    namespace Planetology {
        class Terrain;
    }

    // Ce qui entoure un vaisseau en vol (Simulation::GetFlightState) : planète la plus proche, altitude, air. Pour l'aide
    // au pilotage (ShipInput) et les infos de vol du jeu.
    class FlightSurroundings {
        public:
        // Garde le relief de la dernière planète : il n'est recréé que si elle change.
        FlightState Get(Scene& scene, PhysicsWorld* physics, Entity ship);

        private:
        std::shared_ptr<const Planetology::Terrain> m_terrain; // relief de la planète la plus proche
        std::int32_t m_terrainSeed = 0;
        float m_terrainWidth = 0.0f;
    };
}
