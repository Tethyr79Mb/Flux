#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Scene/Camera.hpp"
#include "Flux/Scene/FlyCameraController.hpp"

#include <memory>
#include <string>

namespace Flux {
    class Renderer;
    class Window;

    struct EngineConfig {
        std::string title = "Flux";
        Math::Vector2i windowSize{1280, 720};
        bool showDebugOverlay = true; // fps, temps de frame et GPU en haut à gauche
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
        Camera& GetCamera() { return m_camera; }

        private:
        std::unique_ptr<Window> m_window;
        std::unique_ptr<Renderer> m_renderer;
        // Provisoire : la caméra et son pilotage iront dans le viewport de l'éditeur.
        Camera m_camera;
        FlyCameraController m_cameraController;
        bool m_quitRequested = false;
        bool m_showDebugOverlay = true;
    };
}
