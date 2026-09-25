#include "Flux/Core/Engine.hpp"

#include "Flux/Platform/Window.hpp"
#include "Flux/Renderer/Renderer.hpp"

#include <cstdio>

namespace Flux {
    Engine::Engine(const EngineConfig& config)
        : m_window(std::make_unique<Window>()) {
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
