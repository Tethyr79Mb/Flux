#pragma once

#include "Flux/Core/Project.hpp"
#include "Flux/Export.hpp"
#include "Flux/Graphics/Camera.hpp"
#include "Flux/Graphics/FlyCameraController.hpp"
#include "Flux/Math/Box3.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"
#include "Flux/Scene/ScreenUi.hpp"
#include "Flux/Scene/Weapons.hpp"
#include "Flux/UI/DrawList.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <string>
#include <vector>
#include <string_view>

namespace Flux {
    class AudioSystem;
    class InputActions;
    class InterfaceLibrary;
    class PhysicsContext;
    class PhysicsWorld;
    class Renderer;
    class Window;

    namespace Planet {
        class Terrain;
    }

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
        bool fullscreen = false; // plein écran sans bordure ; Alt+Entrée ou F11 bascule
        bool showDebugOverlay = true; // panneau « Moteur » : fps, GPU, V-Sync, caméra (le jeu ; l'éditeur a les siens)
        // Interface de la scène (textes à l'écran) sur toute la fenêtre. L'éditeur la dessine lui-même, dans sa vue.
        bool drawScreenUi = true;
        // Écran de chargement après chaque scène chargée en jeu : la scène reste cachée et le jeu arrêté tant que le
        // sol des planètes et ses textures ne sont pas prêts. L'éditeur a déjà sa scène affichée : il s'en passe.
        bool loadingScreen = true;
        // Caméra vol libre (ZQSD + clic droit) quand aucun personnage n'est dirigé par le joueur : l'éditeur seulement.
        // Le jeu arrive directement sur le personnage ; sans personnage (écran titre), la caméra ne bouge pas.
        bool freeCamera = true;
        bool vsync = true;
        bool temporalAA = true; // anticrénelage temporel (TAA)
        float volume = 1.0f; // volume général, 0 à 1
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
        void SetProject(Project project);
        const Project* GetProject() const { return m_project ? &*m_project : nullptr; }
        Project* GetProject() { return m_project ? &*m_project : nullptr; }
        // Lecture des prefabs du projet (assets), pour charger et enregistrer les scènes qui en ont ; sans projet,
        // aucun prefab n'est lisible. Une seule pour tout le projet : les textes lus y restent tant que les fichiers
        // ne changent pas.
        const SceneSerializer::PrefabReader& GetPrefabReader() const { return m_prefabReader; }

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
        // Liaisons des interfaces (champ « Liaison » d'une case, d'un curseur, d'une barre de progression ou d'un
        // texte) : une valeur nommée, lue à chaque image pour l'afficher, et changée par set quand le joueur touche
        // la case (0 ou 1) ou le curseur (sans set : en lecture seule). Le moteur fournit les siennes (« V-Sync »,
        // « Volume général », « Images par seconde ») ; celles du jeu remplacent une valeur du même nom et sont
        // retirées avec ses fonctions quand le module de jeu est déchargé.
        //     engine.BindInterfaceValue("Vie", [] { return g_vie; });
        //     engine.BindInterfaceValue("Sensibilité", [] { return g_sensibilite; }, [](double v) { g_sensibilite = v; });
        //     engine.BindInterfaceText("Nom du vaisseau", [] { return g_nom; });
        void BindInterfaceValue(std::string name, std::function<double()> get, std::function<void(double)> set = {});
        void BindInterfaceText(std::string name, std::function<std::string()> get);
        void UnbindInterfaceValue(std::string_view name);
        // Noms des valeurs liables (moteur puis jeu), pour l'éditeur.
        std::vector<std::string> GetInterfaceValueNames() const;
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
        // Charge tout de suite la scène du fichier (scène de démarrage du jeu) ; pendant le jeu, son monde physique
        // est préparé aussitôt (voir PhysicsWorld::Prepare). En cas d'échec, la scène actuelle reste.
        std::expected<void, std::string> LoadScene(const std::filesystem::path& path);

        // Monde physique du jeu en cours, préparé au lancement du jeu et à chaque changement de scène ; nullptr à
        // l'arrêt. Tous partagent le contexte physique du moteur, créé avec lui. Voir PhysicsWorld, PhysicsContext.
        PhysicsWorld* GetPhysics() { return m_physics.get(); }
        // Son : volume général, sons lancés par le jeu. Voir AudioSystem.
        AudioSystem& GetAudio() { return *m_audio; }

        // Actions d'entrée (« Avancer », « Sauter »...) : le moteur déclare les siennes, le jeu ajoute les siennes
        // (et les retire au déchargement). Les touches choisies par le joueur sont gardées dans Touches.json, à côté
        // de l'exécutable.
        InputActions& GetInputActions() { return *m_inputActions; }
        // Action tenue, ou enfoncée à cette image ; false pendant la saisie dans un champ et pendant qu'une touche
        // est en cours de réassignation.
        bool IsActionDown(std::string_view name) const;
        bool WasActionPressed(std::string_view name) const;
        // Réassignation de la touche principale d'une action (bouton d'interface « Changer une touche ») : la
        // prochaine touche enfoncée la remplace, Échap annule.
        void StartKeyRebinding(std::string action);
        const std::string& GetKeyRebinding() const { return m_rebindingAction; }
        void ResetInputActions();

        // Mise à jour du jeu ; un module de jeu doit la retirer (nullptr) avant d'être déchargé.
        void SetGameUpdate(UpdateFunction update);
        // Arrêter le jeu efface aussi son monde physique : il sera recréé d'après la scène au prochain lancement.
        void SetGameState(GameState state);
        GameState GetGameState() const { return m_gameState; }
        // Écran de chargement affiché (EngineConfig::loadingScreen) : le jeu (physique, module du jeu) attend.
        bool IsLoading() const { return m_loading; }
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
        SceneSerializer::PrefabReader m_prefabReader;
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
        bool m_loadingScreen = true;
        // Écran de chargement : en cours, depuis combien de temps, images de suite sans rien à attendre, et le plus
        // d'attentes vues (pour la barre d'avancement).
        bool m_loading = false;
        double m_loadingSeconds = 0.0;
        std::uint32_t m_loadingQuietFrames = 0;
        std::uint32_t m_loadingMostPending = 0;
        bool m_freeCamera = true;
        std::optional<std::string> m_requestedScene;
        std::shared_ptr<PhysicsContext> m_physicsContext;
        std::unique_ptr<PhysicsWorld> m_physics;
        std::shared_ptr<const Planet::Terrain> m_flightTerrain; // relief de la planète la plus proche (DrawFlightInfo)
        std::int32_t m_flightTerrainSeed = 0;
        // Vitesse réellement parcourue par le vaisseau piloté (DrawFlightInfo) : sa position à l'image d'avant et la
        // vitesse mesurée, lissée.
        Entity m_flightShip = NullEntity;
        Math::WorldPosition m_flightPosition;
        Math::Vector3d m_flightVelocity;
        std::unique_ptr<AudioSystem> m_audio;
        std::unique_ptr<InterfaceLibrary> m_interfaces;
        std::uint64_t m_frame = 0;
        bool m_interfaceWantsMouse = false; // à l'image précédente
        bool m_interfaceMouseNow = false;   // à l'image en cours, d'après les interfaces déjà dessinées
        bool m_cameraInputEnabled = true;
        bool m_interfacePaused = false;    // à l'image précédente
        bool m_interfacePausedNow = false; // à l'image en cours
        bool m_escapeUsed = false;         // Échap a déjà servi à une interface dans cette image
        std::map<std::string, InterfaceFunction, std::less<>> m_interfaceFunctions;
        struct InterfaceValue
        {
            std::function<double()> get{};
            std::function<void(double)> set{};
            std::function<std::string()> getText{}; // texte : prioritaire sur get pour un élément Texte
        };
        std::map<std::string, InterfaceValue, std::less<>> m_engineValues; // fournies par le moteur
        std::map<std::string, InterfaceValue, std::less<>> m_interfaceValues; // fournies par le jeu
        double m_fps = 0.0; // moyenne de la dernière seconde (« Images par seconde »)
        std::unique_ptr<InputActions> m_inputActions;
        std::string m_rebindingAction; // action dont on attend la nouvelle touche
        bool m_keyCaptured = false;    // une touche a servi à la réassignation à cette image : le jeu ne la voit pas
        void CaptureKeyRebinding();
        void SaveInputActions() const;
        InterfaceFunction m_gameQuitHandler;
        // Actions des boutons cliqués pendant le dessin des interfaces, faites à la fin de la frame.
        std::vector<std::pair<Interface::ButtonAction, std::string>> m_interfaceActions;
        // Caméra qui suit le personnage du joueur pendant le jeu, à cette distance (molette). Molette vers l'avant
        // au plus près : vue à la première personne (depuis ses yeux, personnage non dessiné) ; vers l'arrière : retour.
        float m_followDistance = 4.0f;
        bool m_firstPerson = false;
        float m_followShown = -1.0f; // distance réelle, raccourcie par les murs ; négative : pas encore placée
        Entity m_hiddenPlayer = NullEntity; // caméra dans sa tête (vue cockpit, ou coincée contre un mur) : pas dessiné
        bool m_followRotating = false; // clic droit maintenu : la souris tourne autour du personnage
        // Aux commandes : la souris dirige le vaisseau (curseur caché ; Alt maintenu ou un menu de pause le libère).
        // Rotation demandée à cette image, en radians.
        float m_steerYaw = 0.0f;
        float m_steerPitch = 0.0f;
        // Projectiles en vol (Weapons), vidés quand le jeu s'arrête ou qu'une scène se charge.
        std::vector<Projectile> m_projectiles;
        std::vector<ProjectileHit> m_projectileHits; // de l'image, gardés d'une image à l'autre : pas d'allocation
        std::vector<Impact> m_impacts;
        std::vector<std::string> m_destroyedNames;
        Entity m_lockTarget = NullEntity; // cible verrouillée par les armes du vaisseau piloté (DrawFlightInfo)
        std::uint16_t m_activeGroups = 0;  // groupes d'armes en tir automatique (bit 0 : touche 1)
        // Message bref sous les infos de vol (pilote automatique lancé, aucun quai à portée), et son temps restant.
        std::string m_flightMessage;
        double m_flightMessageSeconds = 0.0;
        // Vaisseau où se trouve le joueur, et sa rotation à la dernière image : la caméra tourne avec lui.
        Entity m_followShip = NullEntity;
        Math::Quaternionf m_followShipRotation;
        // Repère où la caméra qui suit le joueur tourne (lacet, tangage) : son y est le haut de l'endroit (le monde, le
        // pont du vaisseau, ou la verticale de la planète), son horizontale celle du joueur. Il tourne avec le
        // vaisseau et se redresse au plus court quand le haut change (on fait le tour d'une planète).
        Math::Quaternionf m_followFrame;
        bool m_followFrameSet = false;
        float m_followYaw = 0.0f;
        float m_followPitch = 0.0f;
        // Aux commandes d'un vaisseau : vue depuis l'œil du pilote, ou de l'extérieur, derrière le vaisseau et tournée
        // avec lui (V ou molette pour changer), et où il regarde (clic droit).
        bool m_cockpitView = true;
        float m_lookYaw = 0.0f;
        float m_lookPitch = 0.0f;
        // Vue extérieure : distance en tailles du vaisseau (molette), et sa boîte (centre dans son repère, rayon),
        // calculée une fois par vaisseau piloté.
        float m_chaseZoom = 1.4f;
        float m_chaseShown = -1.0f; // distance réelle, raccourcie par les murs ; négative : pas encore placée
        Entity m_chaseShip = NullEntity;
        Math::Vector3f m_chaseCenter;
        float m_chaseRadius = 10.0f;

        const InterfaceValue* FindInterfaceValue(std::string_view name) const;
        // Interfaces en jeu : les éléments liés affichent leur valeur, ou la changent (changed : touché par le joueur).
        void SyncInterfaceBindings(std::vector<Interface::Element>& elements, std::uint32_t changed);

        // Composants du moteur qui agissent pendant le jeu (changement de scène), avant la mise à jour du jeu.
        void UpdateGameSystems(double deltaSeconds);
        void LoadRequestedScene();
        // Nouveau monde physique pour la scène actuelle, corps déjà créés.
        void StartPhysics();
        void RunInterfaceActions();
        // Personnage suivi par la caméra (celui du joueur), NullEntity sans jeu en cours ou sans personnage.
        Entity GetFollowedPlayer() const;
        void RotateFollowCamera();
        void PlaceFollowCamera(Entity player, double deltaSeconds);
        // Poste de pilotage d'où la caméra regarde (vue cockpit), NullEntity si elle suit le personnage.
        Entity GetCockpitSeat(Entity player) const;
        void TurnWithShip(Entity player);
        // Où vole un vaisseau par rapport à la planète la plus proche : altitude au-dessus du sol (ou de la mer qui
        // le recouvre), verticale du lieu (unitaire, monde) et densité de l'air (0 dans l'espace, voir ShipInput::air).
        struct FlightState
        {
            bool nearPlanet = false;
            double altitude = 0.0;
            Math::Vector3d up;
            float air = 0.0f;
            double boundary = 0.0; // distance au haut de l'atmosphère, ou à l'altitude sans atmosphère (ShipInput::planetDistance)
            double center = 0.0;   // distance au centre de la planète (ShipInput::planetCenter)
        };
        FlightState GetFlightState(Entity ship);
        // Vitesse, vitesse verticale et altitude du vaisseau piloté, en bas de l'écran.
        void DrawFlightInfo(Entity player, Math::Vector2f screen, double deltaSeconds);
        void ShowFlightMessage(std::string message);
        void BeginLoading();
        // Après le rendu : le chargement est fini quand plus rien n'est attendu pendant quelques images.
        void UpdateLoading(double deltaSeconds);
        void DrawLoadingScreen(Math::Vector2f screen);
        // Lacet et tangage de la caméra suiveuse d'après où regarde la caméra (début du suivi, retour du cockpit).
        void AimFollowCamera();
        // Vue extérieure d'un vaisseau piloté depuis seat ; calcule sa boîte (m_chaseCenter, m_chaseRadius).
        void PlaceChaseCamera(Entity seat, double deltaSeconds);
        void MeasureShip(Entity ship);
        std::optional<std::pair<Math::Vector2f, Math::Vector2f>> DrawInterface(const ScreenUi::InterfaceRequest& request);
    };
}
