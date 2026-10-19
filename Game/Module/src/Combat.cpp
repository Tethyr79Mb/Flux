#include "Combat.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Scene/CombatAI.hpp>
#include <Flux/Scene/Ships.hpp>

namespace Game {
    namespace {
        using namespace Flux;

        // Cible des ennemis : le vaisseau du joueur, aux commandes ou à bord (pas une station où il marche).
        Entity FindTarget(Engine& engine) {
            const Scene& scene = engine.GetScene();
            const Entity player = engine.GetPlayer();
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

        void Steer(Engine& engine, double deltaSeconds) {
            CombatAI::Steer(engine.GetScene(), deltaSeconds, FindTarget(engine));
        }

        void Fire(Engine& engine, double deltaSeconds) {
            CombatAI::Fire(engine.GetScene(), deltaSeconds, FindTarget(engine), engine.GetProjectiles());
        }

        constexpr const char* SteerSystem = "Combat : pilotage des ennemis";
        constexpr const char* FireSystem = "Combat : tirs des ennemis";
    }

    void RegisterCombat(Engine& engine) {
        engine.AddGameSystem(Engine::GameStage::BeforePhysics, SteerSystem, Steer);
        engine.AddGameSystem(Engine::GameStage::AfterPhysics, FireSystem, Fire);
    }

    void UnregisterCombat(Engine& engine) {
        engine.RemoveGameSystem(SteerSystem);
        engine.RemoveGameSystem(FireSystem);
    }
}
