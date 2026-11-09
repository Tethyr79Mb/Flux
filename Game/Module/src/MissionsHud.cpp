#include "Hud.hpp"

#include "GameSim.hpp"
#include "MissionGiver.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/Scene/Picking.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/UI/Context.hpp>
#include <Flux/UI/HudText.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace Game {
    namespace {
        using namespace Flux;

        constexpr float WindowWidth = 440.0f;
        constexpr std::size_t LineLength = 58; // caractères par ligne de l'ordre de mission, à la largeur de la fenêtre

        // Texte coupé en lignes aux espaces (FluxUI n'a pas de texte à la ligne automatique).
        std::vector<std::string> WrapLines(std::string_view text) {
            std::vector<std::string> lines;
            std::string line;
            std::size_t start = 0;
            while (start <= text.size()) {
                const std::size_t end = std::min(text.find(' ', start), text.size());
                const std::string_view word = text.substr(start, end - start);
                if (!line.empty() && line.size() + 1 + word.size() > LineLength) {
                    lines.push_back(std::move(line));
                    line.clear();
                }
                if (!line.empty()) {
                    line += ' ';
                }
                line += word;
                start = end + 1;
            }
            if (!line.empty()) {
                lines.push_back(std::move(line));
            }
            return lines;
        }

        std::string NameOf(const Scene& scene, Entity entity) {
            const auto* name = scene.IsValid(entity) ? scene.TryGet<NameComponent>(entity) : nullptr;
            return name ? name->name : std::string("le donneur");
        }

        void DrawWindow(Engine& engine, MissionState& missions, Entity player, Math::Vector2f screen) {
            Scene& scene = engine.GetScene();
            if (missions.window == NullEntity || !scene.IsValid(missions.window) || !scene.TryGet<MissionGiverComponent>(missions.window)
                || Missions::FindGiver(scene, player) != missions.window) {
                missions.window = NullEntity;
                return;
            }
            const Entity giver = missions.window;
            const MissionGiverComponent& mission = scene.Get<MissionGiverComponent>(giver);
            bool open = true;
            UI::Context& ui = engine.GetUi();
            if (ui.BeginPanel(NameOf(scene, giver) + "###mission", {(screen.x - WindowWidth) * 0.5f, screen.y * 0.3f}, WindowWidth,
                              0.0f, &open)) {
                ui.Text(mission.title);
                ui.Separator();
                for (const std::string& line : WrapLines(mission.briefing)) {
                    ui.Text(line);
                }
                ui.Separator();
                if (missions.mission && missions.mission->giver == giver) {
                    const int alive = Missions::CountAlive(scene, missions.mission->enemies);
                    ui.Text(std::format("Mission en cours : {} ennemi{} restant{}", alive, alive > 1 ? "s" : "", alive > 1 ? "s" : ""));
                    ui.TextDisabled("Aux commandes : nez vers son marqueur, J pour le warp.");
                    if (ui.Button("Abandonner")) {
                        Missions::Abandon(engine.GetSimulation(), player);
                        open = false;
                    }
                } else if (missions.mission) {
                    ui.TextDisabled("Une autre mission est déjà en cours.");
                } else {
                    const int count = std::clamp(mission.enemyCount, 1, 50);
                    ui.Text(std::format("Cible : {} ennemi{}, à {} de la station", count, count > 1 ? "s" : "",
                                        FormatDistance(mission.distance)));
                    if (ui.Button("Accepter")) {
                        // Un échec est annoncé en haut de l'écran.
                        (void)Missions::Accept(engine.GetSimulation(), player, giver);
                        open = false;
                    }
                    ui.SameLine();
                    if (ui.Button("Refuser")) {
                        open = false;
                    }
                }
            }
            ui.EndPanel();
            if (!open) {
                missions.window = NullEntity;
            }
        }

        void DrawInfo(Engine& engine, MissionState& missions, Entity player, Math::Vector2f screen, double deltaSeconds) {
            const Scene& scene = engine.GetScene();
            UI::DrawList& list = engine.GetRenderer().GetOverlay();
            constexpr float Size = 18.0f;
            const auto box = [&](const std::string& text, float y, Math::LinearColor color) {
                const Math::Vector2f extent = list.MeasureText(text, Size);
                const Math::Vector2f topLeft{(screen.x - extent.x) * 0.5f, y};
                list.AddRoundedRect(topLeft - Math::Vector2f{14.0f, 8.0f}, topLeft + extent + Math::Vector2f{14.0f, 8.0f},
                                    {0.0f, 0.0f, 0.0f, 0.45f}, 8.0f);
                list.AddText(topLeft, text, Size, color);
            };
            // À pied près d'un donneur, fenêtre fermée : comment lui parler.
            if (missions.window == NullEntity) {
                if (const Entity giver = Missions::FindGiver(scene, player); giver != NullEntity) {
                    box("F : parler à " + NameOf(scene, giver), screen.y * 0.62f, {1.0f, 1.0f, 1.0f, 1.0f});
                }
            }
            float y = 20.0f;
            if (missions.messageSeconds > 0.0) {
                missions.messageSeconds -= deltaSeconds;
                box(missions.message, y, {0.6f, 1.0f, 0.6f, 1.0f});
                y += 44.0f;
            }
            if (!missions.mission) {
                return;
            }
            // Mission en cours : ennemis restants, distance au plus proche, et une marque sur lui à l'écran.
            constexpr Math::LinearColor MissionColor{1.0f, 0.8f, 0.3f, 1.0f};
            const Camera& camera = engine.GetCamera();
            const Math::WorldPosition eye = camera.position;
            Entity nearest = NullEntity;
            double nearestDistance = 0.0;
            int alive = 0;
            for (const Entity enemy : missions.mission->enemies) {
                if (!scene.IsValid(enemy)) {
                    continue;
                }
                ++alive;
                const double distance = Math::Length(Math::Delta(eye, scene.GetWorldPosition(enemy)));
                if (nearest == NullEntity || distance < nearestDistance) {
                    nearest = enemy;
                    nearestDistance = distance;
                }
            }
            const int total = static_cast<int>(missions.mission->enemies.size());
            box(std::format("Mission : {}   {}/{} détruits   {}", missions.mission->title, total - alive, total, FormatDistance(nearestDistance)),
                y, MissionColor);
            if (nearest == NullEntity) {
                return;
            }
            const Math::Vector3d offset = Math::Delta(eye, scene.GetWorldPosition(nearest));
            if (const auto point = ScreenProjection(camera, {}, screen).ToScreen(offset)) {
                constexpr float Half = 10.0f; // losange
                const Math::Vector2f c = *point;
                list.AddLine({c.x, c.y - Half}, {c.x + Half, c.y}, MissionColor, 2.0f);
                list.AddLine({c.x + Half, c.y}, {c.x, c.y + Half}, MissionColor, 2.0f);
                list.AddLine({c.x, c.y + Half}, {c.x - Half, c.y}, MissionColor, 2.0f);
                list.AddLine({c.x - Half, c.y}, {c.x, c.y - Half}, MissionColor, 2.0f);
            }
        }

        // Missions du joueur de cet écran.
        void DrawHud(Engine& engine, double deltaSeconds) {
            const Entity player = engine.GetPlayer();
            PlayerStateComponent* state = FindPlayerState(engine.GetScene(), player);
            if (!state) {
                return;
            }
            const Math::Vector2i size = engine.GetWindow().GetSize();
            const Math::Vector2f screen{static_cast<float>(size.x), static_cast<float>(size.y)};
            DrawInfo(engine, state->missions, player, screen, deltaSeconds);
            DrawWindow(engine, state->missions, player, screen);
        }

        constexpr const char* HudSystem = "Missions : affichage";
        constexpr const char* SaveData = "missions";
    }

    void RegisterMissionsHud(Engine& engine) {
        engine.AddGameSystem(Engine::GameStage::Hud, HudSystem, [](Engine& e, double dt) { DrawHud(e, dt); });
        // Mission du joueur de cet écran (partie solo).
        engine.AddGameSaveData(
            SaveData,
            [](Engine& e) {
                const PlayerStateComponent* state = FindPlayerState(e.GetScene(), e.GetPlayer());
                return state ? Missions::Save(e.GetScene(), state->missions) : std::string{};
            },
            [](Engine& e, std::string_view data) { Missions::Load(e.GetSimulation(), e.GetPlayer(), data); });
    }

    void UnregisterMissionsHud(Engine& engine) {
        engine.RemoveGameSystem(HudSystem);
        engine.RemoveGameSaveData(SaveData);
    }
}
