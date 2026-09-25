#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Scene/Camera.hpp"
#include "Flux/Scene/FlyCameraController.hpp"
#include "Flux/Scene/Scene.hpp"

#include <functional>
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
        // Appelée à chaque frame, après le déplacement de la caméra et avant le rendu.
        using UpdateFunction = std::function<void(Engine& engine, double deltaSeconds)>;

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
        Scene& GetScene() { return m_scene; }

        // Mise à jour du jeu ; un module de jeu doit la retirer (nullptr) avant d'être déchargé.
        void SetGameUpdate(UpdateFunction update);

        private:
        std::unique_ptr<Window> m_window;
        std::unique_ptr<Renderer> m_renderer;
        Scene m_scene;
        UpdateFunction m_gameUpdate;
        // Provisoire : la caméra et son pilotage iront dans le viewport de l'éditeur.
        Camera m_camera;
        FlyCameraController m_cameraController;
        bool m_quitRequested = false;
        bool m_showDebugOverlay = true;
    };
}
