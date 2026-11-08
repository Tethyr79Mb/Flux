#include "Combat.hpp"

#include "CombatAI.hpp"

#include <Flux/Core/Simulation.hpp>
#include <Flux/Scene/Ships.hpp>

namespace Game::Combat {
    namespace {
        using namespace Flux;

        // Cible des ennemis : le vaisseau du joueur, aux commandes ou à bord (pas une station où il marche).
        Entity FindTarget(const Simulation& simulation) {
            const Scene& scene = simulation.GetScene();
            const Entity player = simulation.GetPlayer();
            if (player == NullEntity) {
                return NullEntity;
            }
            if (const Entity piloted = Ships::GetPilotedShip(scene, player); piloted != NullEntity) {
                return piloted;
            }
            const Entity aboard = scene.GetShip(player);
            const auto* ship = aboard != NullEntity ? scene.TryGet<ShipComponent>(aboard) : nullptr;
            return ship && ship->maxSpeed > 0.0f ? aboard : NullEntity;
        }
    }

    void Steer(Simulation& simulation, double deltaSeconds) {
        CombatAI::Steer(simulation.GetScene(), deltaSeconds, FindTarget(simulation));
    }

    void Fire(Simulation& simulation, double deltaSeconds) {
        CombatAI::Fire(simulation.GetScene(), deltaSeconds, FindTarget(simulation), simulation.GetProjectiles());
    }
}
