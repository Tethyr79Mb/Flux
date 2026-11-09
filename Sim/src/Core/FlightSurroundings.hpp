#pragma once

#include "Flux/Scene/Components.hpp"
#include "Flux/Scene/Ships.hpp"

#include <cstdint>
#include <memory>
#include <vector>

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
        // Garde le relief des dernières planètes vues (un vaisseau par joueur, chacun près de la sienne).
        FlightState Get(Scene& scene, PhysicsWorld* physics, Entity ship);

        private:
        static constexpr std::size_t MaxReliefs = 8;
        struct Relief
        {
            std::shared_ptr<const Planetology::Terrain> terrain;
            std::int32_t seed = 0;
            float width = 0.0f;
        };
        std::vector<Relief> m_reliefs;
    };
}
