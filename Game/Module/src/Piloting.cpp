#include "Piloting.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Input/InputActions.hpp>
#include <Flux/Scene/Components.hpp>
#include <Flux/Scene/Scene.hpp>

#include <array>
#include <string_view>

namespace Game {
    namespace {
        using namespace Flux;

        constexpr const char* PilotSystem = "Pilotage : commandes";

        PilotActions g_actions; // identifiants, les mêmes pour tous les joueurs

        // Étape Interaction (avant les sièges, comme au temps du moteur) : assis aux commandes, les touches pilotent.
        void Pilot(Engine& engine, double) {
            Scene& scene = engine.GetScene();
            const Entity player = engine.GetPlayer();
            const auto* character = player != NullEntity ? scene.TryGet<CharacterComponent>(player) : nullptr;
            if (!character || character->seat == NullEntity) {
                return;
            }
            const PlayerCommand& command = engine.GetPlayerCommand(player);
            engine.SetPilotInput(ReadShipInput(command, g_actions));
            if (command.WasPressed(g_actions.level)) {
                if (const Entity ship = Ships::GetPilotedShip(scene, player); ship != NullEntity) {
                    ShipComponent& component = scene.Get<ShipComponent>(ship);
                    component.levelAssist = !component.levelAssist;
                    Log::Info("Stabilisation à plat : {}", component.levelAssist ? "oui" : "non");
                }
            }
        }
    }

    PilotActions DeclarePilotActions(InputActions& actions) {
        // Comme Star Citizen : Espace monte, Ctrl descend, X freine ; Maj boost et Tab mode voyage, comme X4.
        PilotActions ids;
        ids.forward = actions.GetId("Avancer");
        ids.back = actions.GetId("Reculer");
        ids.left = actions.GetId("Aller à gauche");
        ids.right = actions.GetId("Aller à droite");
        ids.brake = actions.Add("Freiner", "Vaisseau", Key::X);
        ids.boost = actions.Add("Boost", "Vaisseau", Key::LeftShift);
        ids.travel = actions.Add("Mode voyage", "Vaisseau", Key::Tab);
        ids.up = actions.Add("Monter", "Vaisseau", Key::Space);
        ids.down = actions.Add("Descendre", "Vaisseau", Key::LeftControl);
        ids.rollLeft = actions.Add("Rouler à gauche", "Vaisseau", Key::Q);
        ids.rollRight = actions.Add("Rouler à droite", "Vaisseau", Key::E);
        ids.level = actions.Add("Stabiliser à plat", "Vaisseau", Key::H);
        return ids;
    }

    void RemovePilotActions(InputActions& actions) {
        for (const std::string_view name : {"Freiner", "Boost", "Mode voyage", "Monter", "Descendre", "Rouler à gauche",
                                            "Rouler à droite", "Stabiliser à plat"}) {
            actions.Remove(name);
        }
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

    void RegisterPiloting(Engine& engine) {
        g_actions = DeclarePilotActions(engine.GetInputActions());
        engine.AddGameSystem(Engine::GameStage::Interaction, PilotSystem, Pilot);
    }

    void UnregisterPiloting(Engine& engine) {
        RemovePilotActions(engine.GetInputActions());
        engine.RemoveGameSystem(PilotSystem);
    }
}
