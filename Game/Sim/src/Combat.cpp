#include "Combat.hpp"

#include "CombatAI.hpp"

#include <Flux/Core/Simulation.hpp>
#include <Flux/Scene/Ships.hpp>

#include <algorithm>

namespace Game::Combat {
    namespace {
        using namespace Flux;

        // Cible des ennemis : le vaisseau d'un joueur, aux commandes ou à bord (pas une station où il marche).
        Entity FindTarget(const Scene& scene, Entity player) {
            if (const Entity piloted = Ships::GetPilotedShip(scene, player); piloted != NullEntity) {
                return piloted;
            }
            const Entity aboard = scene.GetShip(player);
            const auto* ship = aboard != NullEntity ? scene.TryGet<ShipComponent>(aboard) : nullptr;
            return ship && ship->maxSpeed > 0.0f ? aboard : NullEntity;
        }
    }

    void Steer(Simulation& simulation, std::vector<Entity>& targets, double deltaSeconds) {
        const Scene& scene = simulation.GetScene();
        targets.clear();
        for (const Entity player : simulation.GetPlayers()) {
            // Deux joueurs dans le même vaisseau : une seule cible.
            if (const Entity target = FindTarget(scene, player); target != NullEntity && !std::ranges::contains(targets, target)) {
                targets.push_back(target);
            }
        }
        CombatAI::Steer(simulation.GetScene(), deltaSeconds, targets);
    }

    void Fire(Simulation& simulation, double deltaSeconds) {
        CombatAI::Fire(simulation.GetScene(), deltaSeconds, simulation.GetProjectiles());
    }
}
