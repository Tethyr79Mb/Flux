#include "Flux/Core/Engine.hpp"

#include "Flux/Platform/Window.hpp"

#include <cstdio>

namespace Flux {
    Engine::Engine(const EngineConfig& config)
        : m_window(std::make_unique<Window>()) {
        if (!m_window->Create(config.title, config.windowSize)) {
            std::fputs("Flux: impossible de créer la fenêtre\n", stderr);
            m_quitRequested = true;
        }
    }

    Engine::~Engine() = default;

    int Engine::Run() {
        if (m_quitRequested) {
            return 1;
        }

        while (!m_quitRequested && m_window->IsOpen()) {
            m_window->HandleEvents();
            // Mise à jour et rendu viendront ici.
        }
        return 0;
    }

    void Engine::RequestQuit() {
        m_quitRequested = true;
    }

    Window& Engine::GetWindow() {
        return *m_window;
    }
}
