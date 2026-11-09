#include "Piloting.hpp"

#include <Flux/Core/Log.hpp>
#include <Flux/Core/Simulation.hpp>
#include <Flux/Scene/Components.hpp>
#include <Flux/Scene/Scene.hpp>

namespace Game::Piloting {
    using namespace Flux;

    PilotActions DeclareActions(CommandActions& actions) {
        PilotActions ids;
        ids.forward = actions.Declare(CharacterActions::Forward);
        ids.back = actions.Declare(CharacterActions::Back);
        ids.left = actions.Declare(CharacterActions::Left);
        ids.right = actions.Declare(CharacterActions::Right);
        ids.brake = actions.Declare(Brake);
        ids.boost = actions.Declare(Boost);
        ids.travel = actions.Declare(Travel);
        ids.up = actions.Declare(Up);
        ids.down = actions.Declare(Down);
        ids.rollLeft = actions.Declare(RollLeft);
        ids.rollRight = actions.Declare(RollRight);
        ids.level = actions.Declare(Level);
        return ids;
    }

    ShipInput ReadShipInput(const PlayerCommand& command, const PilotActions& actions) {
        ShipInput input;
        input.thrust = command.GetAxis(actions.forward, actions.back);
        input.strafe = command.GetAxis(actions.right, actions.left);
        input.lift = command.GetAxis(actions.up, actions.down);
        input.roll = command.GetAxis(actions.rollRight, actions.rollLeft);
        input.brake = command.IsDown(actions.brake);
        input.boost = command.IsDown(actions.boost);
        input.travel = command.WasPressed(actions.travel);
        return input;
    }

    void Pilot(Simulation& simulation, const PilotActions& actions) {
        Scene& scene = simulation.GetScene();
        for (const Entity player : simulation.GetPlayers()) {
            if (scene.Get<CharacterComponent>(player).seat == NullEntity) {
                continue;
            }
            const PlayerCommand& command = simulation.GetPlayerCommand(player);
            simulation.SetPilotInput(player, ReadShipInput(command, actions));
            if (command.WasPressed(actions.level)) {
                if (const Entity ship = Ships::GetPilotedShip(scene, player); ship != NullEntity) {
                    ShipComponent& component = scene.Get<ShipComponent>(ship);
                    component.levelAssist = !component.levelAssist;
                    Log::Info("Stabilisation à plat : {}", component.levelAssist ? "oui" : "non");
                }
            }
        }
    }
}
