#include "Flux/Core/Engine.hpp"

#include "Flux/Platform/Window.hpp"
#include "Flux/Renderer/Renderer.hpp"
#include "Flux/UI/DrawList.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>

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

        // Distance lisible : mètres, kilomètres, puis unités astronomiques.
        std::string FormatDistance(double meters) {
            const double magnitude = std::abs(meters);
            if (magnitude < 1.0e4) {
                return std::format("{:.1f} m", meters);
            }
            if (magnitude < 1.0e9) {
                return std::format("{:.1f} km", meters / 1.0e3);
            }
            return std::format("{:.3f} ua", meters / 1.495978707e11);
        }

        void DrawDebugOverlay(UI::DrawList& overlay, const FrameStats& stats, std::string_view deviceName,
                              const Camera& camera, const FlyCameraController& controller) {
            constexpr float TextSize = 16.0f;
            constexpr float Margin = 8.0f;
            constexpr float Padding = 8.0f;

            const std::string text = std::format(
                "{:.0f} fps · Durée d'image : {:.2f} ms\nGPU : {}\n"
                "Caméra : x {}  y {}  z {}\nVitesse : {}/s (molette)\n"
                "Clic droit maintenu : souris pour regarder, ZQSD, E / A pour monter / descendre, Maj pour accélérer",
                stats.GetFps(), stats.GetFrameMilliseconds(), deviceName,
                FormatDistance(camera.position.x), FormatDistance(camera.position.y), FormatDistance(camera.position.z),
                FormatDistance(controller.GetSpeed()));
            const Math::Vector2f size = overlay.MeasureText(text, TextSize);
            overlay.AddRect({Margin, Margin},
                            {Margin + size.x + 2.0f * Padding, Margin + size.y + 2.0f * Padding},
                            {0.0f, 0.0f, 0.0f, 0.6f});
            overlay.AddText({Margin + Padding, Margin + Padding}, text, TextSize, {1.0f, 1.0f, 1.0f, 1.0f});
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

        // Un peu en arrière et au-dessus de la grille de test, légèrement vers le bas.
        m_camera.position = {0.0, 6.0, 30.0};
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

            m_cameraController.Update(m_camera, *m_window, deltaSeconds);
            m_renderer->SetCamera(m_camera);

            if (m_showDebugOverlay) {
                DrawDebugOverlay(m_renderer->GetOverlay(), stats, m_renderer->GetDeviceName(), m_camera, m_cameraController);
            }

            if (!m_renderer->RenderFrame()) {
                return 1;
            }
        }
        return 0;
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
}
