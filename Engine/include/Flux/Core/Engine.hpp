#pragma once

#include "Flux/Core/Project.hpp"
#include "Flux/Export.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Scene/Camera.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/FlyCameraController.hpp"
#include "Flux/Scene/Scene.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <string>

namespace Flux {
    class Renderer;
    class Window;

    namespace UI {
        class Context;
    }

    struct EngineConfig {
        std::string title = "Flux";
        Math::Vector2i windowSize{1280, 720};
        bool showDebugOverlay = true; // panneau « Moteur » : fps, GPU, V-Sync, caméra
    };

    // État du jeu : l'éditeur démarre à l'arrêt (on édite la scène), Game.exe démarre en cours.
    enum class GameState {
        Stopped, // mise à jour du jeu non appelée : ni animation, ni panneaux du jeu
        Running,
        Paused,  // mise à jour appelée avec un temps écoulé nul : le jeu est figé mais ses panneaux restent
    };

    class FLUX_API Engine {
        public:
        // Appelée à chaque frame, après le déplacement de la caméra et avant le rendu ; peut décrire des panneaux FluxUI.
        using UpdateFunction = std::function<void(Engine& engine, double deltaSeconds)>;
        // Demande de fermeture de la fenêtre : retourne false pour l'annuler (modifications à enregistrer...).
        using CloseFunction = std::function<bool(Engine& engine)>;

        explicit Engine(const EngineConfig& config = {});
        ~Engine();

        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;

        // Boucle principale ; retourne le code de sortie du programme.
        int Run();
        void RequestQuit();

        Window& GetWindow();
        Renderer& GetRenderer();
        // Interface FluxUI de la frame en cours (panneaux décrits dans la fonction de mise à jour du jeu).
        UI::Context& GetUi();
        Camera& GetCamera() { return m_camera; }
        Scene& GetScene() { return m_scene; }
        // Types de composants sauvegardés et affichés dans l'inspecteur : ceux du moteur, puis ceux du jeu
        // (à enregistrer dans FluxRegisterGame, avant de charger une scène qui les utilise).
        ComponentRegistry& GetComponents() { return m_components; }

        // Projet ouvert (nullptr sans projet) : dossier des assets, scène de démarrage.
        void SetProject(Project project) { m_project = std::move(project); }
        const Project* GetProject() const { return m_project ? &*m_project : nullptr; }

        // Mise à jour du jeu ; un module de jeu doit la retirer (nullptr) avant d'être déchargé.
        void SetGameUpdate(UpdateFunction update);
        void SetGameState(GameState state) { m_gameState = state; }
        GameState GetGameState() const { return m_gameState; }
        // Outils (panneaux de l'éditeur), appelés après le jeu : ils voient la scène telle que le jeu l'a laissée.
        void SetEditorUpdate(UpdateFunction update);
        // Appelée quand l'utilisateur ferme la fenêtre ; sans fonction, la fenêtre se ferme toujours.
        void SetCloseRequestHandler(CloseFunction handler);

        private:
        std::unique_ptr<Window> m_window;
        std::unique_ptr<Renderer> m_renderer;
        std::unique_ptr<UI::Context> m_ui;
        ComponentRegistry m_components;
        std::optional<Project> m_project;
        Scene m_scene;
        UpdateFunction m_gameUpdate;
        GameState m_gameState = GameState::Running;
        UpdateFunction m_editorUpdate;
        CloseFunction m_closeRequestHandler;
        // Provisoire : la caméra et son pilotage iront dans le viewport de l'éditeur.
        Camera m_camera;
        FlyCameraController m_cameraController;
        bool m_quitRequested = false;
        bool m_showDebugOverlay = true;
    };
}
