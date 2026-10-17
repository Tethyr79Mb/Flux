// Missions du moteur : parler à un donneur (F), accepter, suivre le groupe d'ennemis jusqu'à sa destruction.
#include "Flux/Core/Engine.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Window.hpp"
#include "Flux/Renderer/Renderer.hpp"
#include "Flux/Scene/Missions.hpp"
#include "Flux/Scene/Picking.hpp"
#include "Flux/UI/Context.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace Flux {
    namespace {
        constexpr double MessageSeconds = 5.0;
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

        // Nombre à la française, en m ou km selon sa taille.
        std::string FormatDistance(double meters) {
            std::string text = meters >= 10000.0 ? std::format("{:.1f} km", meters / 1000.0) : std::format("{:.0f} m", meters);
            std::ranges::replace(text, '.', ',');
            return text;
        }

        std::string NameOf(const Scene& scene, Entity entity) {
            const auto* name = scene.IsValid(entity) ? scene.TryGet<NameComponent>(entity) : nullptr;
            return name ? name->name : std::string("le donneur");
        }
    }

    void Engine::ResetMissions() {
        m_mission.reset();
        m_missionWindow = NullEntity;
        m_missionMessageSeconds = 0.0;
    }

    void Engine::UpdateMissions(Entity player, bool& interact) {
        // Tous les ennemis détruits : mission accomplie.
        if (m_mission && Missions::CountAlive(m_scene, m_mission->enemies) == 0) {
            m_missionMessage = "Mission accomplie : " + m_mission->title;
            m_missionMessageSeconds = MessageSeconds;
            Log::Info("{}", m_missionMessage);
            m_mission.reset();
        }
        const Entity giver = Missions::FindGiver(m_scene, player);
        if (interact && giver != NullEntity) {
            m_missionWindow = m_missionWindow == giver ? NullEntity : giver; // F encore : la referme
            interact = false;
        } else if (m_missionWindow != NullEntity && giver != m_missionWindow) {
            m_missionWindow = NullEntity; // parti, ou donneur disparu
        }
    }

    void Engine::DrawMissionWindow(Entity player, Math::Vector2f screen) {
        if (m_missionWindow == NullEntity || !m_scene.IsValid(m_missionWindow)
            || !m_scene.TryGet<MissionGiverComponent>(m_missionWindow) || Missions::FindGiver(m_scene, player) != m_missionWindow) {
            m_missionWindow = NullEntity;
            return;
        }
        const Entity giver = m_missionWindow;
        const MissionGiverComponent& mission = m_scene.Get<MissionGiverComponent>(giver);
        bool open = true;
        UI::Context& ui = *m_ui;
        if (ui.BeginPanel(NameOf(m_scene, giver) + "###mission", {(screen.x - WindowWidth) * 0.5f, screen.y * 0.3f}, WindowWidth, 0.0f,
                          &open)) {
            ui.Text(mission.title);
            ui.Separator();
            for (const std::string& line : WrapLines(mission.briefing)) {
                ui.Text(line);
            }
            ui.Separator();
            if (m_mission && m_mission->giver == giver) {
                const int alive = Missions::CountAlive(m_scene, m_mission->enemies);
                ui.Text(std::format("Mission en cours : {} ennemi{} restant{}", alive, alive > 1 ? "s" : "", alive > 1 ? "s" : ""));
                if (ui.Button("Abandonner")) {
                    // Le groupe repart avec la mission : rien ne reste à abattre pour rien.
                    for (const Entity enemy : m_mission->enemies) {
                        if (m_scene.IsValid(enemy)) {
                            m_scene.DestroyEntity(enemy);
                        }
                    }
                    m_mission.reset();
                    open = false;
                }
            } else if (m_mission) {
                ui.TextDisabled("Une autre mission est déjà en cours.");
            } else {
                const int count = std::clamp(mission.enemyCount, 1, 50);
                ui.Text(std::format("Cible : {} ennemi{}, à {} de la station", count, count > 1 ? "s" : "",
                                    FormatDistance(mission.distance)));
                if (ui.Button("Accepter")) {
                    if (auto enemies = Missions::SpawnEnemies(m_scene, m_components, GetPrefabReader(), giver)) {
                        m_mission = ActiveMission{giver, mission.title, std::move(*enemies)};
                        Log::Info("Mission acceptée : {}", mission.title);
                    } else {
                        Log::Warning("Mission « {} » : {}", mission.title, enemies.error());
                        m_missionMessage = "Mission impossible : ennemis introuvables";
                        m_missionMessageSeconds = MessageSeconds;
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
            m_missionWindow = NullEntity;
        }
    }

    void Engine::DrawMissionInfo(Entity player, Math::Vector2f screen, double deltaSeconds) {
        UI::DrawList& list = m_renderer->GetOverlay();
        constexpr float Size = 18.0f;
        const auto box = [&](const std::string& text, float y, Math::LinearColor color) {
            const Math::Vector2f extent = list.MeasureText(text, Size);
            const Math::Vector2f topLeft{(screen.x - extent.x) * 0.5f, y};
            list.AddRoundedRect(topLeft - Math::Vector2f{14.0f, 8.0f}, topLeft + extent + Math::Vector2f{14.0f, 8.0f},
                                {0.0f, 0.0f, 0.0f, 0.45f}, 8.0f);
            list.AddText(topLeft, text, Size, color);
        };
        // À pied près d'un donneur, fenêtre fermée : comment lui parler.
        if (m_missionWindow == NullEntity) {
            if (const Entity giver = Missions::FindGiver(m_scene, player); giver != NullEntity) {
                box("F : parler à " + NameOf(m_scene, giver), screen.y * 0.62f, {1.0f, 1.0f, 1.0f, 1.0f});
            }
        }
        float y = 20.0f;
        if (m_missionMessageSeconds > 0.0) {
            m_missionMessageSeconds -= deltaSeconds;
            box(m_missionMessage, y, {0.6f, 1.0f, 0.6f, 1.0f});
            y += 44.0f;
        }
        if (!m_mission) {
            return;
        }
        // Mission en cours : ennemis restants, distance au plus proche, et une marque sur lui à l'écran.
        constexpr Math::LinearColor MissionColor{1.0f, 0.8f, 0.3f, 1.0f};
        const Math::WorldPosition eye = m_camera.position;
        Entity nearest = NullEntity;
        double nearestDistance = 0.0;
        int alive = 0;
        for (const Entity enemy : m_mission->enemies) {
            if (!m_scene.IsValid(enemy)) {
                continue;
            }
            ++alive;
            const double distance = Math::Length(Math::Delta(eye, m_scene.GetWorldPosition(enemy)));
            if (nearest == NullEntity || distance < nearestDistance) {
                nearest = enemy;
                nearestDistance = distance;
            }
        }
        const int total = static_cast<int>(m_mission->enemies.size());
        box(std::format("Mission : {}   {}/{} détruits   {}", m_mission->title, total - alive, total, FormatDistance(nearestDistance)), y,
            MissionColor);
        if (nearest == NullEntity) {
            return;
        }
        const Math::Vector3d offset = Math::Delta(eye, m_scene.GetWorldPosition(nearest));
        if (const auto point = ScreenProjection(m_camera, {}, screen).ToScreen(offset)) {
            constexpr float Half = 10.0f; // losange
            const Math::Vector2f c = *point;
            list.AddLine({c.x, c.y - Half}, {c.x + Half, c.y}, MissionColor, 2.0f);
            list.AddLine({c.x + Half, c.y}, {c.x, c.y + Half}, MissionColor, 2.0f);
            list.AddLine({c.x, c.y + Half}, {c.x - Half, c.y}, MissionColor, 2.0f);
            list.AddLine({c.x - Half, c.y}, {c.x, c.y - Half}, MissionColor, 2.0f);
        }
    }
}
