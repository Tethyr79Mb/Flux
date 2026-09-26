#include "Flux/Core/Engine.hpp"

#include "Flux/Platform/Window.hpp"
#include "Flux/Renderer/Renderer.hpp"
#include "Flux/UI/Context.hpp"
#include "Flux/UI/DrawList.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <utility>

namespace Flux {
    namespace {
        using Clock = std::chrono::steady_clock;

        // Moyenne sur une fenêtre glissante d'une demi-seconde : lisible, sans chiffres qui clignotent.
        class FrameStats {
            public:
            void Tick(Clock::time_point now) {
                ++m_frames;
                const std::chrono::duration<double> elapsed = now - m_windowStart;
                if (elapsed.count() >= 0.5) {
                    m_fps = m_frames / elapsed.count();
                    m_frameMilliseconds = 1000.0 * elapsed.count() / m_frames;
                    m_frames = 0;
                    m_windowStart = now;
                }
            }

            double GetFps() const { return m_fps; }
            double GetFrameMilliseconds() const { return m_frameMilliseconds; }

            private:
            Clock::time_point m_windowStart = Clock::now();
            int m_frames = 0;
            double m_fps = 0.0;
            double m_frameMilliseconds = 0.0;
        };

        // Distance lisible : mètres, kilomètres, unités astronomiques puis années-lumière.
        std::string FormatDistance(double meters) {
            const double magnitude = std::abs(meters);
            if (magnitude < 1.0e4) {
                return std::format("{:.1f} m", meters);
            }
            if (magnitude < 1.0e9) {
                return std::format("{:.1f} km", meters / 1.0e3);
            }
            if (magnitude < 1.0e15) {
                return std::format("{:.3f} ua", meters / 1.495978707e11);
            }
            return std::format("{:.4f} al", meters / 9.4607304725808e15);
        }

        void DrawEnginePanel(UI::Context& ui, Renderer& renderer, const FrameStats& stats, Camera& camera,
                             FlyCameraController& controller) {
            if (ui.BeginPanel("Moteur", {8.0f, 8.0f}, 360.0f)) {
                ui.LabelText("Images/s", std::format("{:.0f} ({:.2f} ms)", stats.GetFps(), stats.GetFrameMilliseconds()));
                ui.LabelText("GPU", renderer.GetDeviceName());
                bool vsync = renderer.IsVSyncEnabled();
                if (ui.Checkbox("V-Sync", vsync)) {
                    renderer.SetVSync(vsync);
                }
                ui.LabelText("Présentation", renderer.GetPresentModeName());

                ui.Separator();
                ui.LabelText("Secteur", std::format("{}  {}  {}", camera.position.sector.x, camera.position.sector.y,
                                                    camera.position.sector.z));
                Math::Vector3d offset = camera.position.offset;
                if (ui.DragDouble3("Position (m)", offset, 0.1, UI::Context::NoMin, UI::Context::NoMax, 2)) {
                    camera.position.offset = offset;
                    camera.position.Normalize();
                }
                // Pas proportionnel à la valeur : 1 % par pixel, utilisable du cm/s à l'année-lumière par seconde.
                double speed = controller.GetSpeed();
                if (ui.DragDouble("Vitesse (m/s)", speed, speed * 0.01, 0.0, UI::Context::NoMax, 2)) {
                    controller.SetSpeed(speed);
                }
                ui.LabelText("Soit", FormatDistance(controller.GetSpeed()) + "/s");

                ui.Separator();
                ui.TextDisabled("Clic droit maintenu : regarder autour\n"
                                "ZQSD : avancer, E / A : monter / descendre\n"
                                "Maj : accélérer, molette : régler la vitesse");
            }
            ui.EndPanel();
        }
    }

    Engine::Engine(const EngineConfig& config)
        : m_window(std::make_unique<Window>()),
          m_showDebugOverlay(config.showDebugOverlay) {
        if (!m_window->Create(config.title, config.windowSize)) {
            std::fputs("Flux : impossible de créer la fenêtre\n", stderr);
            m_quitRequested = true;
            return;
        }

        auto renderer = Renderer::Create(*m_window);
        if (!renderer) {
            std::fprintf(stderr, "Flux : %s\n", renderer.error().c_str());
            m_quitRequested = true;
            return;
        }
        m_renderer = std::move(renderer.value());
        m_ui = std::make_unique<UI::Context>(m_renderer->GetOverlay().GetFont());

        // Un peu en arrière et au-dessus de la grille de test, légèrement vers le bas.
        m_camera.position.offset = {0.0, 6.0, 30.0};
        m_camera.pitch = -0.2f;
    }

    // Défini ici, où Renderer est complet, pour que unique_ptr<Renderer> puisse le détruire.
    Engine::~Engine() = default;

    int Engine::Run() {
        if (m_quitRequested) {
            return 1;
        }

        FrameStats stats;
        Clock::time_point previousFrame = Clock::now();
        while (!m_quitRequested && m_window->IsOpen()) {
            m_window->HandleEvents();
            if (!m_window->IsOpen()) {
                break;
            }

            const Math::Vector2i size = m_window->GetSize();
            if (size.x == 0 || size.y == 0) {
                m_window->WaitEvents(); // fenêtre réduite : on dort jusqu'au prochain événement
                continue;
            }

            const Clock::time_point now = Clock::now();
            // Plafonné : après un blocage (déplacement de fenêtre, point d'arrêt), la caméra ne fait pas un bond.
            const double deltaSeconds = std::min(std::chrono::duration<double>(now - previousFrame).count(), 0.1);
            previousFrame = now;
            stats.Tick(now);

            // Souris ignorée par l'UI pendant le vol libre : le curseur est caché et immobile.
            m_ui->BeginFrame(m_window->GetInput(), {static_cast<float>(size.x), static_cast<float>(size.y)}, deltaSeconds,
                             !m_window->IsCursorLocked());
            m_cameraController.Update(m_camera, *m_window, deltaSeconds, !m_ui->WantsMouse(), !m_ui->WantsKeyboard());
            if (m_gameUpdate) {
                m_gameUpdate(*this, deltaSeconds);
            }
            if (m_showDebugOverlay) {
                DrawEnginePanel(*m_ui, *m_renderer, stats, m_camera, m_cameraController);
            }
            m_ui->EndFrame(m_renderer->GetOverlay());

            if (!m_renderer->RenderFrame(m_scene, m_camera)) {
                return 1;
            }
        }
        return 0;
    }

    void Engine::SetGameUpdate(UpdateFunction update) {
        m_gameUpdate = std::move(update);
    }

    void Engine::RequestQuit() {
        m_quitRequested = true;
    }

    Window& Engine::GetWindow() {
        return *m_window;
    }

    Renderer& Engine::GetRenderer() {
        return *m_renderer;
    }

    UI::Context& Engine::GetUi() {
        return *m_ui;
    }
}
