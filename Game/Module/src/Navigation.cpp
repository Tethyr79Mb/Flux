#include "Navigation.hpp"

#include "Docking.hpp"
#include "Warp.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Input/InputActions.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/Scene/Picking.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/Ships.hpp>
#include <Flux/UI/HudText.hpp>

#include <algorithm>
#include <format>
#include <vector>

namespace Game {
    namespace {
        using namespace Flux;

        constexpr const char* AutopilotAction = "Pilote automatique";
        constexpr const char* WarpAction = "Warp";
        constexpr const char* InfoSource = "Navigation"; // ligne des infos de vol (Engine::SetFlightInfoExtra)

        // Destinations du vaisseau piloté (recalculées à chaque image, mémoire gardée), celle que vise son nez (-1 :
        // aucune), le nom de celle vers laquelle il file (vide hors warp), l'objectif de mission et la ligne d'amarrage.
        std::vector<Warp::Destination> g_destinations;
        int g_aim = -1;
        std::string g_warpName;
        std::string g_objectiveName;
        std::optional<Math::WorldPosition> g_objective;
        std::string g_info;

        std::string GetDestinationName(const Scene& scene, const Warp::Destination& destination) {
            if (destination.entity == NullEntity) {
                return g_objectiveName.empty() ? std::string("Objectif") : g_objectiveName;
            }
            const auto* name = scene.IsValid(destination.entity) ? scene.TryGet<NameComponent>(destination.entity) : nullptr;
            return name ? name->name : std::string("Destination");
        }

        void ToggleAutopilot(Engine& engine, Entity ship) {
            Scene& scene = engine.GetScene();
            Warp::Stop(scene, ship); // L en warp : il en sort et cherche un quai
            g_warpName.clear();
            switch (Docking::Toggle(scene, ship)) {
            case Docking::ToggleResult::Docking:
            case Docking::ToggleResult::Leaving: break; // l'étape s'affiche dans les infos de vol
            case Docking::ToggleResult::Cancelled: engine.ShowFlightMessage("Pilote automatique coupé"); break;
            case Docking::ToggleResult::NoDock:
                engine.ShowFlightMessage(std::format("Aucun quai libre à moins de {:.0f} km", Docking::Range / 1000.0));
                break;
            }
        }

        // Destinations à jour, visée, et J : partir vers la destination visée, ou couper le warp.
        void UpdateWarp(Engine& engine, Entity ship, bool toggle) {
            g_aim = -1;
            if (ship == NullEntity) {
                g_destinations.clear();
                return;
            }
            Scene& scene = engine.GetScene();
            Warp::FindDestinations(scene, ship, g_destinations);
            if (g_objective) {
                g_destinations.push_back({NullEntity, *g_objective, Warp::StationExit});
            }
            const auto* drive = scene.TryGet<WarpDriveComponent>(ship);
            const bool warping = drive && drive->stage != WarpStage::Off;
            // Arrivé, ou arrêté par une planète sur le chemin.
            if (!warping && !g_warpName.empty()) {
                engine.ShowFlightMessage("Sortie du warp : " + g_warpName);
                g_warpName.clear();
            }
            if (!warping) {
                g_aim = Warp::Aim(scene, ship, g_destinations);
            }
            if (!toggle) {
                return;
            }
            if (warping) {
                Warp::Stop(scene, ship);
                g_warpName.clear();
                engine.ShowFlightMessage("Warp coupé");
                return;
            }
            if (g_aim < 0) {
                engine.ShowFlightMessage("Warp : tournez le nez vers une destination");
                return;
            }
            const Warp::Destination& destination = g_destinations[static_cast<std::size_t>(g_aim)];
            switch (Warp::Engage(scene, ship, destination)) {
            case Warp::EngageResult::Started: g_warpName = GetDestinationName(scene, destination); break;
            case Warp::EngageResult::Unavailable: engine.ShowFlightMessage("Warp impossible ici (posé, amarré, près d'une planète, sans moteur de warp)"); break;
            case Warp::EngageResult::TooClose: engine.ShowFlightMessage("Trop près pour le warp : mode voyage (Tab)"); break;
            }
        }

        // Après le pilote (Ships::Update), avant l'IA de combat et la physique, comme quand c'était dans le moteur.
        void Travel(Engine& engine, double deltaSeconds) {
            Scene& scene = engine.GetScene();
            const Entity player = engine.GetPlayer();
            const Entity ship = player != NullEntity ? Ships::GetPilotedShip(scene, player) : NullEntity;
            if (ship != NullEntity && engine.WasActionPressed(AutopilotAction)) {
                ToggleAutopilot(engine, ship);
            }
            Docking::Update(scene, deltaSeconds);
            UpdateWarp(engine, ship, ship != NullEntity && engine.WasActionPressed(WarpAction));
            Warp::Update(scene, deltaSeconds);
            g_info = ship != NullEntity ? Docking::Describe(scene, ship) : std::string{};
            engine.SetFlightInfoExtra(InfoSource, g_info);
        }

        // Destinations du warp à l'écran, la visée en clair avec la touche ; en warp, la sienne et un bandeau.
        void DrawHud(Engine& engine, double) {
            const Scene& scene = engine.GetScene();
            const Entity player = engine.GetPlayer();
            const Entity ship = player != NullEntity ? Ships::GetPilotedShip(scene, player) : NullEntity;
            if (ship == NullEntity || g_destinations.empty()) {
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
            for (int i = 0; i < static_cast<int>(g_destinations.size()); ++i) {
                const Warp::Destination& destination = g_destinations[static_cast<std::size_t>(i)];
                const bool aimed = i == g_aim;
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
                std::string text = std::format("{}  {}", GetDestinationName(scene, destination), FormatDistance(Math::Length(offset)));
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
                                         ? std::format("Warp vers {} : charge {:.0f} %", g_warpName,
                                                       std::min(drive->charge / Warp::ChargeTime, 1.0f) * 100.0f)
                                         : std::format("Warp vers {} : {}", g_warpName, FormatDistance(Warp::GetRemaining(scene, ship)));
            const Math::Vector2f extent = list.MeasureText(text, 18.0f);
            const Math::Vector2f topLeft{(screen.x - extent.x) * 0.5f, screen.y * 0.7f};
            list.AddRoundedRect(topLeft - Math::Vector2f{14.0f, 8.0f}, topLeft + extent + Math::Vector2f{14.0f, 8.0f}, {0.0f, 0.0f, 0.0f, 0.45f}, 8.0f);
            list.AddText(topLeft, text, 18.0f, AimColor);
        }

        void Reset(Engine&, double) {
            g_destinations.clear();
            g_aim = -1;
            g_warpName.clear();
            g_objective.reset();
            g_objectiveName.clear();
            g_info.clear();
        }

        constexpr const char* TravelSystem = "Navigation : amarrage et warp";
        constexpr const char* HudSystem = "Navigation : affichage";
        constexpr const char* ResetSystem = "Navigation : oubli";
    }

    void SetWarpObjective(std::string name, std::optional<Math::WorldPosition> position) {
        g_objectiveName = position ? std::move(name) : std::string{};
        g_objective = position;
    }

    void RegisterNavigation(Engine& engine) {
        // Près d'une station : se faire conduire au quai libre le plus proche ; amarré : repartir (choix de Bruno).
        engine.GetInputActions().Add(AutopilotAction, "Vaisseau", Key::L);
        // Vers la destination que vise le nez (station, planète, objectif de mission) ; en warp : le couper.
        engine.GetInputActions().Add(WarpAction, "Vaisseau", Key::J);
        engine.AddGameSystem(Engine::GameStage::BeforePhysics, TravelSystem, Travel);
        engine.AddGameSystem(Engine::GameStage::Hud, HudSystem, DrawHud);
        engine.AddGameSystem(Engine::GameStage::Reset, ResetSystem, Reset);
    }

    void UnregisterNavigation(Engine& engine) {
        engine.RemoveGameSystem(TravelSystem);
        engine.RemoveGameSystem(HudSystem);
        engine.RemoveGameSystem(ResetSystem);
        engine.GetInputActions().Remove(AutopilotAction);
        engine.GetInputActions().Remove(WarpAction);
        engine.SetFlightInfoExtra(InfoSource, {});
        Reset(engine, 0.0);
    }
}
