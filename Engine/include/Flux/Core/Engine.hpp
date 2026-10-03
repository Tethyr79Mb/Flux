#pragma once

#include "Flux/Core/Project.hpp"
#include "Flux/Export.hpp"
#include "Flux/Graphics/Camera.hpp"
#include "Flux/Graphics/FlyCameraController.hpp"
#include "Flux/Math/Box3.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/ScreenUi.hpp"
#include "Flux/UI/DrawList.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <string>
#include <vector>
#include <string_view>

namespace Flux {
    class InterfaceLibrary;
    class PhysicsWorld;
    class Renderer;
    class Window;

    namespace UI {
        class Context;
    }

    namespace Interface {
        struct Document;
        struct Element;
        enum class ButtonAction : std::uint8_t;
    }

    struct EngineConfig {
        std::string title = "Flux";
        Math::Vector2i windowSize{1280, 720}; // taille de la fenêtre restaurée si maximized
        bool maximized = false;
        bool showDebugOverlay = true; // panneau « Moteur » : fps, GPU, V-Sync, caméra (le jeu ; l'éditeur a les siens)
        // Interface de la scène (textes à l'écran) sur toute la fenêtre. L'éditeur la dessine lui-même, dans sa vue.
        bool drawScreenUi = true;
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
        // Fonction C++ appelée par un bouton d'interface (action « Fonction C++ »), ou à la place de « Quitter le jeu ».
        using InterfaceFunction = std::function<void(Engine& engine)>;

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
        FlyCameraController& GetCameraController() { return m_cameraController; }
        Scene& GetScene() { return m_scene; }
        // Types de composants sauvegardés et affichés dans l'inspecteur : ceux du moteur, puis ceux du jeu
        // (à enregistrer dans FluxRegisterGame, avant de charger une scène qui les utilise).
        ComponentRegistry& GetComponents() { return m_components; }

        // Projet ouvert (nullptr sans projet) : dossier des assets, scène de démarrage.
        void SetProject(Project project) { m_project = std::move(project); }
        const Project* GetProject() const { return m_project ? &*m_project : nullptr; }
        Project* GetProject() { return m_project ? &*m_project : nullptr; }

        // Image du projet (chemin relatif au dossier des assets, « textures/logo.png ») chargée pour l'overlay :
        // voir Renderer::LoadTexture. nullopt sans projet, ou si le fichier est absent ou illisible.
        std::optional<UI::TextureInfo> LoadAssetTexture(std::string_view relativePath);
        // Interface du projet (fichier .fluxui, chemin relatif au dossier des assets), relue quand le fichier change :
        // l'éditeur d'interface enregistre, le jeu se met à jour. nullptr sans projet, ou si le fichier est absent ou
        // illisible (l'erreur va dans la console).
        const Interface::Document* LoadInterface(std::string_view relativePath);
        // Réglages de ScreenUi::Draw pour la scène : images et interfaces du projet. Pendant le jeu, les interfaces
        // réagissent à la souris (cases, curseurs, boutons, fenêtres déplacées).
        ScreenUi::DrawOptions GetScreenUiOptions();
        // Vrai si la souris était sur une interface du jeu à l'image précédente : ses clics ne vont pas au jeu.
        bool InterfaceWantsMouse() const { return m_interfaceWantsMouse; }
        // Éditeur : false fige la caméra en vol libre (la souris et le clavier servent à l'éditeur d'interface).
        void SetCameraInputEnabled(bool enabled) { m_cameraInputEnabled = enabled; }

        // Fonction appelée par les boutons dont l'action est « Fonction C++ » et la cible ce nom (remplace celle du
        // même nom). Appelée à la fin de la frame, hors du dessin : elle peut changer la scène. Le moteur les retire
        // toutes quand le module de jeu est déchargé (leur code est dans sa DLL).
        void RegisterInterfaceFunction(std::string name, InterfaceFunction function);
        void UnregisterInterfaceFunction(std::string_view name);
        void ClearInterfaceFunctions();
        // Remplace la fermeture de la fenêtre pour l'action « Quitter le jeu » (l'éditeur arrête le mode Jouer).
        void SetGameQuitHandler(InterfaceFunction handler) { m_gameQuitHandler = std::move(handler); }
        // Interfaces en jeu (une copie par entité, créée quand elle est dessinée la première fois) : ouvre ou ferme
        // l'élément de ce nom dans toutes ; false si aucune n'en a.
        bool SetInterfaceWindowOpen(std::string_view name, bool open);
        // Premier élément de ce nom dans les interfaces en jeu (lire une case, un curseur ; changer un texte) ;
        // nullptr hors du jeu ou si aucune n'en a. Valable jusqu'à la fin de la frame.
        Interface::Element* FindInterfaceElement(std::string_view name);
        // Une fenêtre ouverte « Met le jeu en pause » à l'image précédente : le jeu reçoit un temps écoulé nul.
        bool IsInterfacePausing() const { return m_interfacePaused; }

        // Boîte englobante du maillage d'une entité, dans son repère (avant sa transformation) : le cube unité pour
        // un cube (MeshRendererComponent) ou un modèle sans fichier lisible, celle du modèle sinon (chargé au besoin) ;
        // à défaut, celle de sa capsule de personnage ou de sa forme de collision. nullopt si l'entité n'a rien de
        // tout cela. Sert à la sélection dans la vue et à son contour.
        std::optional<Math::Box3f> GetMeshBounds(Entity entity);
        // Noms des animations du modèle 3D de l'entité (ModelComponent) ; vide sans modèle ou sans animation.
        std::vector<std::string> GetAnimationNames(Entity entity);

        // Remplace la scène par celle du fichier (chemin relatif au dossier des assets, « scenes/Demo.scene.json »)
        // à la fin de la frame en cours : ce qui se passe pendant la frame voit encore l'ancienne scène.
        // Utilisé par SceneChangeComponent, et par le code du jeu. Sans effet si le jeu est arrêté (éditeur hors du
        // mode Jouer) ; en cas d'échec (fichier absent...), la scène actuelle reste et l'erreur va dans la console.
        void RequestScene(std::string relativePath);

        // Monde physique du jeu en cours (créé à la première image de jeu, après chaque changement de scène) ;
        // nullptr à l'arrêt. Voir PhysicsWorld.
        PhysicsWorld* GetPhysics() { return m_physics.get(); }

        // Mise à jour du jeu ; un module de jeu doit la retirer (nullptr) avant d'être déchargé.
        void SetGameUpdate(UpdateFunction update);
        // Arrêter le jeu efface aussi son monde physique : il sera recréé d'après la scène au prochain lancement.
        void SetGameState(GameState state);
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
        bool m_drawScreenUi = true;
        std::optional<std::string> m_requestedScene;
        std::unique_ptr<PhysicsWorld> m_physics;
        std::unique_ptr<InterfaceLibrary> m_interfaces;
        std::uint64_t m_frame = 0;
        bool m_interfaceWantsMouse = false; // à l'image précédente
        bool m_interfaceMouseNow = false;   // à l'image en cours, d'après les interfaces déjà dessinées
        bool m_cameraInputEnabled = true;
        bool m_interfacePaused = false;    // à l'image précédente
        bool m_interfacePausedNow = false; // à l'image en cours
        bool m_escapeUsed = false;         // Échap a déjà servi à une interface dans cette image
        std::map<std::string, InterfaceFunction, std::less<>> m_interfaceFunctions;
        InterfaceFunction m_gameQuitHandler;
        // Actions des boutons cliqués pendant le dessin des interfaces, faites à la fin de la frame.
        std::vector<std::pair<Interface::ButtonAction, std::string>> m_interfaceActions;
        // Caméra qui suit le personnage du joueur pendant le jeu, à cette distance (molette).
        float m_followDistance = 4.0f;
        float m_followShown = -1.0f; // distance réelle, raccourcie par les murs ; négative : pas encore placée
        Entity m_hiddenPlayer = NullEntity; // caméra dans sa tête (vue cockpit, ou coincée contre un mur) : pas dessiné
        bool m_followRotating = false; // clic droit maintenu : la souris tourne autour du personnage
        // Vaisseau où se trouve le joueur, et sa rotation à la dernière image : la caméra tourne avec lui.
        Entity m_followShip = NullEntity;
        Math::Quaternionf m_followShipRotation;
        // Aux commandes d'un vaisseau : vue depuis l'œil du pilote (V : vue extérieure), et où il regarde (clic droit).
        bool m_cockpitView = true;
        float m_lookYaw = 0.0f;
        float m_lookPitch = 0.0f;

        // Composants du moteur qui agissent pendant le jeu (changement de scène), avant la mise à jour du jeu.
        void UpdateGameSystems(double deltaSeconds);
        void LoadRequestedScene();
        void RunInterfaceActions();
        // Personnage suivi par la caméra (celui du joueur), NullEntity sans jeu en cours ou sans personnage.
        Entity GetFollowedPlayer() const;
        void RotateFollowCamera();
        void PlaceFollowCamera(Entity player, double deltaSeconds);
        // Poste de pilotage d'où la caméra regarde (vue cockpit), NullEntity si elle suit le personnage.
        Entity GetCockpitSeat(Entity player) const;
        void TurnWithShip(Entity player);
        std::optional<std::pair<Math::Vector2f, Math::Vector2f>> DrawInterface(const ScreenUi::InterfaceRequest& request);
    };
}
