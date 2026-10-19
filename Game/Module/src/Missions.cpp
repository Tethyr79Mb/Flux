#include "Missions.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/Scene/Missions.hpp>
#include <Flux/Scene/Picking.hpp>
#include <Flux/UI/Context.hpp>
#include <Flux/UI/HudText.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace Game {
    namespace {
        using namespace Flux;

        constexpr double MessageSeconds = 5.0;
        constexpr float WindowWidth = 440.0f;
        constexpr std::size_t LineLength = 58; // caractères par ligne de l'ordre de mission, à la largeur de la fenêtre

        // Mission acceptée auprès d'un donneur, une seule à la fois : ses ennemis encore à détruire.
        struct ActiveMission
        {
            Entity giver = NullEntity;
            std::string title;
            std::vector<Entity> enemies;
            Math::WorldPosition area; // où ils sont apparus : destination du warp
        };

        std::optional<ActiveMission> g_mission;
        Entity g_window = NullEntity; // donneur dont la fenêtre est ouverte
        std::string g_message;        // « Mission accomplie », en haut de l'écran
        double g_messageSeconds = 0.0;

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

        void SetMission(Engine& engine, std::optional<ActiveMission> mission) {
            g_mission = std::move(mission);
            engine.SetWarpObjective(g_mission ? "Mission : " + g_mission->title : std::string{},
                                    g_mission ? std::optional{g_mission->area} : std::nullopt);
        }

        void Reset(Engine&, double) {
            g_mission.reset(); // l'objectif du warp est effacé par le moteur avec la scène
            g_window = NullEntity;
            g_messageSeconds = 0.0;
        }

        void Interact(Engine& engine, double) {
            Scene& scene = engine.GetScene();
            // Tous les ennemis détruits : mission accomplie.
            if (g_mission && Missions::CountAlive(scene, g_mission->enemies) == 0) {
                g_message = "Mission accomplie : " + g_mission->title;
                g_messageSeconds = MessageSeconds;
                Log::Info("{}", g_message);
                SetMission(engine, std::nullopt);
            }
            const Entity player = engine.GetPlayer();
            const auto* character = player != NullEntity ? scene.TryGet<CharacterComponent>(player) : nullptr;
            if (character && character->seat != NullEntity) {
                g_window = NullEntity; // assis : F sert au siège
                return;
            }
            const Entity giver = Missions::FindGiver(scene, player);
            if (giver != NullEntity && engine.ConsumeInteraction()) {
                g_window = g_window == giver ? NullEntity : giver; // F encore : la referme
            } else if (g_window != NullEntity && giver != g_window) {
                g_window = NullEntity; // parti, ou donneur disparu
            }
        }

        void DrawWindow(Engine& engine, Entity player, Math::Vector2f screen) {
            Scene& scene = engine.GetScene();
            if (g_window == NullEntity || !scene.IsValid(g_window) || !scene.TryGet<MissionGiverComponent>(g_window)
                || Missions::FindGiver(scene, player) != g_window) {
                g_window = NullEntity;
                return;
            }
            const Entity giver = g_window;
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
                if (g_mission && g_mission->giver == giver) {
                    const int alive = Missions::CountAlive(scene, g_mission->enemies);
                    ui.Text(std::format("Mission en cours : {} ennemi{} restant{}", alive, alive > 1 ? "s" : "", alive > 1 ? "s" : ""));
                    ui.TextDisabled("Aux commandes : nez vers son marqueur, J pour le warp.");
                    if (ui.Button("Abandonner")) {
                        // Le groupe repart avec la mission : rien ne reste à abattre pour rien.
                        for (const Entity enemy : g_mission->enemies) {
                            if (scene.IsValid(enemy)) {
                                scene.DestroyEntity(enemy);
                            }
                        }
                        SetMission(engine, std::nullopt);
                        open = false;
                    }
                } else if (g_mission) {
                    ui.TextDisabled("Une autre mission est déjà en cours.");
                } else {
                    const int count = std::clamp(mission.enemyCount, 1, 50);
                    ui.Text(std::format("Cible : {} ennemi{}, à {} de la station", count, count > 1 ? "s" : "",
                                        FormatDistance(mission.distance)));
                    if (ui.Button("Accepter")) {
                        if (auto enemies = Missions::SpawnEnemies(scene, engine.GetComponents(), engine.GetPrefabReader(), giver)) {
                            SetMission(engine, ActiveMission{giver, mission.title, std::move(*enemies), Missions::GetTargetArea(scene, giver)});
                            Log::Info("Mission acceptée : {}", mission.title);
                        } else {
                            Log::Warning("Mission « {} » : {}", mission.title, enemies.error());
                            g_message = "Mission impossible : ennemis introuvables";
                            g_messageSeconds = MessageSeconds;
                        }
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
                g_window = NullEntity;
            }
        }

        void DrawInfo(Engine& engine, Entity player, Math::Vector2f screen, double deltaSeconds) {
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
            if (g_window == NullEntity) {
                if (const Entity giver = Missions::FindGiver(scene, player); giver != NullEntity) {
                    box("F : parler à " + NameOf(scene, giver), screen.y * 0.62f, {1.0f, 1.0f, 1.0f, 1.0f});
                }
            }
            float y = 20.0f;
            if (g_messageSeconds > 0.0) {
                g_messageSeconds -= deltaSeconds;
                box(g_message, y, {0.6f, 1.0f, 0.6f, 1.0f});
                y += 44.0f;
            }
            if (!g_mission) {
                return;
            }
            // Mission en cours : ennemis restants, distance au plus proche, et une marque sur lui à l'écran.
            constexpr Math::LinearColor MissionColor{1.0f, 0.8f, 0.3f, 1.0f};
            const Camera& camera = engine.GetCamera();
            const Math::WorldPosition eye = camera.position;
            Entity nearest = NullEntity;
            double nearestDistance = 0.0;
            int alive = 0;
            for (const Entity enemy : g_mission->enemies) {
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
            const int total = static_cast<int>(g_mission->enemies.size());
            box(std::format("Mission : {}   {}/{} détruits   {}", g_mission->title, total - alive, total, FormatDistance(nearestDistance)),
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

        void DrawHud(Engine& engine, double deltaSeconds) {
            const Entity player = engine.GetPlayer();
            const Math::Vector2i size = engine.GetWindow().GetSize();
            const Math::Vector2f screen{static_cast<float>(size.x), static_cast<float>(size.y)};
            DrawInfo(engine, player, screen, deltaSeconds);
            DrawWindow(engine, player, screen);
        }

        constexpr const char* InteractSystem = "Missions : parler";
        constexpr const char* HudSystem = "Missions : affichage";
        constexpr const char* ResetSystem = "Missions : oubli";
    }

    void RegisterMissions(Engine& engine) {
        engine.AddGameSystem(Engine::GameStage::Interaction, InteractSystem, Interact);
        engine.AddGameSystem(Engine::GameStage::Hud, HudSystem, DrawHud);
        engine.AddGameSystem(Engine::GameStage::Reset, ResetSystem, Reset);
    }

    void UnregisterMissions(Engine& engine) {
        engine.RemoveGameSystem(InteractSystem);
        engine.RemoveGameSystem(HudSystem);
        engine.RemoveGameSystem(ResetSystem);
        g_mission.reset();
        g_window = NullEntity;
    }
}
