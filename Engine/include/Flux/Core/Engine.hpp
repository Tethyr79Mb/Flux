#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector2.hpp"

#include <memory>
#include <string>

namespace Flux {
    class Renderer;
    class Window;

    struct EngineConfig {
        std::string title = "Flux";
        Math::Vector2i windowSize{1280, 720};
    };

    class FLUX_API Engine {
        public:
        explicit Engine(const EngineConfig& config = {});
        ~Engine();

        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;

        // Boucle principale ; retourne le code de sortie du programme.
        int Run();
        void RequestQuit();

        Window& GetWindow();
        Renderer& GetRenderer();

        private:
        std::unique_ptr<Window> m_window;
        std::unique_ptr<Renderer> m_renderer;
        bool m_quitRequested = false;
    };
}
