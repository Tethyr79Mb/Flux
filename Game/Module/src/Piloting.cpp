#include "Piloting.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Input/InputActions.hpp>
#include <Flux/Scene/Components.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/Ships.hpp>

#include <array>
#include <string_view>

namespace Game {
    namespace {
        using namespace Flux;

        constexpr const char* PilotSystem = "Pilotage : commandes";

        struct ShipAction
        {
            std::string_view name;
            Key key;
        };
        // Comme Star Citizen : Espace monte, Ctrl descend, X freine ; Maj boost et Tab mode voyage, comme X4. Avancer,
        // reculer et glisser sont les touches de déplacement du moteur (celles du personnage).
        constexpr std::array ShipActions{
            ShipAction{"Freiner", Key::X},          ShipAction{"Boost", Key::LeftShift},
            ShipAction{"Mode voyage", Key::Tab},    ShipAction{"Monter", Key::Space},
            ShipAction{"Descendre", Key::LeftControl}, ShipAction{"Rouler à gauche", Key::Q},
            ShipAction{"Rouler à droite", Key::E},  ShipAction{"Stabiliser à plat", Key::H},
        };

        // Étape Interaction (avant les sièges, comme au temps du moteur) : assis aux commandes, les touches pilotent.
        void Pilot(Engine& engine, double) {
            Scene& scene = engine.GetScene();
            const Entity player = engine.GetPlayer();
            const auto* character = player != NullEntity ? scene.TryGet<CharacterComponent>(player) : nullptr;
            if (!character || character->seat == NullEntity) {
                return;
            }
            const auto axis = [&](std::string_view positive, std::string_view negative) {
                return (engine.IsActionDown(positive) ? 1.0f : 0.0f) - (engine.IsActionDown(negative) ? 1.0f : 0.0f);
            };
            ShipInput input;
            input.thrust = axis("Avancer", "Reculer");
            input.strafe = axis("Aller à droite", "Aller à gauche");
            input.lift = axis("Monter", "Descendre");
            input.roll = axis("Rouler à droite", "Rouler à gauche");
            input.brake = engine.IsActionDown("Freiner");
            input.boost = engine.IsActionDown("Boost");
            input.travel = engine.WasActionPressed("Mode voyage");
            engine.SetPilotInput(input);
            if (engine.WasActionPressed("Stabiliser à plat")) {
                if (const Entity ship = Ships::GetPilotedShip(scene, player); ship != NullEntity) {
                    ShipComponent& component = scene.Get<ShipComponent>(ship);
                    component.levelAssist = !component.levelAssist;
                    Log::Info("Stabilisation à plat : {}", component.levelAssist ? "oui" : "non");
                }
            }
        }
    }

    void RegisterPiloting(Engine& engine) {
        for (const ShipAction& action : ShipActions) {
            engine.GetInputActions().Add(std::string(action.name), "Vaisseau", action.key);
        }
        engine.AddGameSystem(Engine::GameStage::Interaction, PilotSystem, Pilot);
    }

    void UnregisterPiloting(Engine& engine) {
        for (const ShipAction& action : ShipActions) {
            engine.GetInputActions().Remove(action.name);
        }
        engine.RemoveGameSystem(PilotSystem);
    }
}
