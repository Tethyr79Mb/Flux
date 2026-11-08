#include "Hud.hpp"

#include "GameSim.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/Scene/Picking.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/Ships.hpp>
#include <Flux/UI/HudText.hpp>

#include <algorithm>
#include <format>

namespace Game {
    namespace {
        using namespace Flux;

        // Destinations du warp à l'écran, la visée en clair avec la touche ; en warp, la sienne et un bandeau.
        void DrawHud(Engine& engine, const NavigationState& state) {
            const Scene& scene = engine.GetScene();
            const Entity player = engine.GetPlayer();
            const Entity ship = player != NullEntity ? Ships::GetPilotedShip(scene, player) : NullEntity;
            if (ship == NullEntity || state.destinations.empty()) {
                return;
            }
            const auto* drive = scene.TryGet<WarpDriveComponent>(ship);
            const WarpStage stage = drive ? drive->stage : WarpStage::Off;
            const Math::Vector2i size = engine.GetWindow().GetSize();
            const Math::Vector2f screen{static_cast<float>(size.x), static_cast<float>(size.y)};
            const Camera& camera = engine.GetCamera();
            UI::DrawList& list = engine.GetRenderer().GetOverlay();
            constexpr float Size = 16.0f;
            constexpr Math::LinearColor DestinationColor{0.55f, 0.8f, 1.0f, 0.85f};
            constexpr Math::LinearColor AimColor{0.75f, 0.95f, 1.0f, 1.0f};
            const ScreenProjection projection(camera, {}, screen);
            for (int i = 0; i < static_cast<int>(state.destinations.size()); ++i) {
                const Warp::Destination& destination = state.destinations[static_cast<std::size_t>(i)];
                const bool aimed = i == state.aim;
                if (stage != WarpStage::Off && destination.position != drive->target) {
                    continue;
                }
                const Math::Vector3d offset = Math::Delta(camera.position, destination.position);
                const auto point = projection.ToScreen(offset);
                if (!point) {
                    continue;
                }
                const Math::LinearColor color = aimed || stage != WarpStage::Off ? AimColor : DestinationColor;
                list.AddCircleOutline(*point, aimed ? 12.0f : 8.0f, color, aimed ? 2.0f : 1.5f);
                std::string text = std::format("{}  {}", Navigation::GetDestinationName(scene, state, destination), FormatDistance(Math::Length(offset)));
                if (aimed) {
                    text += "   J : warp";
                }
                list.AddText(*point + Math::Vector2f{16.0f, -Size * 0.5f}, text, Size, color);
            }
            if (stage == WarpStage::Off) {
                return;
            }
            // Bandeau au centre, sous le réticule : charge, puis distance restante.
            const std::string text = stage == WarpStage::Charging
                                         ? std::format("Warp vers {} : charge {:.0f} %", state.warpName,
                                                       std::min(drive->charge / Warp::ChargeTime, 1.0f) * 100.0f)
                                         : std::format("Warp vers {} : {}", state.warpName, FormatDistance(Warp::GetRemaining(scene, ship)));
            const Math::Vector2f extent = list.MeasureText(text, 18.0f);
            const Math::Vector2f topLeft{(screen.x - extent.x) * 0.5f, screen.y * 0.7f};
            list.AddRoundedRect(topLeft - Math::Vector2f{14.0f, 8.0f}, topLeft + extent + Math::Vector2f{14.0f, 8.0f}, {0.0f, 0.0f, 0.0f, 0.45f}, 8.0f);
            list.AddText(topLeft, text, 18.0f, AimColor);
        }

        constexpr const char* HudSystem = "Navigation : affichage";
    }

    void RegisterNavigationHud(Engine& engine, std::shared_ptr<SimState> state) {
        engine.AddGameSystem(Engine::GameStage::Hud, HudSystem,
                             [state = std::move(state)](Engine& e, double) { DrawHud(e, state->navigation); });
    }

    void UnregisterNavigationHud(Engine& engine) {
        engine.RemoveGameSystem(HudSystem);
    }
}
