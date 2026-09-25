#include "Flux/Core/Engine.hpp"

#include "Flux/Platform/Window.hpp"
#include "Flux/Renderer/Renderer.hpp"
#include "Flux/UI/DrawList.hpp"

#include <chrono>
#include <cstdio>
#include <format>

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

        void DrawDebugOverlay(UI::DrawList& overlay, const FrameStats& stats, std::string_view deviceName) {
            constexpr float TextSize = 16.0f;
            constexpr float Margin = 8.0f;
            constexpr float Padding = 8.0f;

            const std::string text = std::format("{:.0f} fps · Durée d'image : {:.2f} ms\nGPU : {}",
                                                 stats.GetFps(), stats.GetFrameMilliseconds(), deviceName);
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
    }

    // Défini ici, où Renderer est complet, pour que unique_ptr<Renderer> puisse le détruire.
    Engine::~Engine() = default;

    int Engine::Run() {
        if (m_quitRequested) {
            return 1;
        }

        FrameStats stats;
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

            stats.Tick(Clock::now());
            if (m_showDebugOverlay) {
                DrawDebugOverlay(m_renderer->GetOverlay(), stats, m_renderer->GetDeviceName());
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
