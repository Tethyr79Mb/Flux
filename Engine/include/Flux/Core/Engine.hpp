#pragma once

#include "Flux/Core/Project.hpp"
#include "Flux/Export.hpp"
#include "Flux/Graphics/Camera.hpp"
#include "Flux/Graphics/FlyCameraController.hpp"
#include "Flux/Math/Box3.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Planetology/GroundDetail.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"
#include "Flux/Scene/ScreenUi.hpp"
#include "Flux/Scene/Warp.hpp"
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
    class FlightHud;
    class InputActions;
    class InterfaceBindings;
    class InterfaceLibrary;
    class LoadingScreen;
    class PhysicsContext;
    class PhysicsWorld;
    class PlayerCamera;
    class QuickSave;
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
        GroundDetail groundDetail = DefaultGroundDetail;
        float volume = 1.0f; // volume général, 0 à 1
    };

    // État du jeu : l'éditeur démarre à l'arrêt (on édite la scène), Ars Solaris.exe démarre en cours.
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
        // Police du projet (chemin relatif au dossier des assets, « Polices/Orbitron.ttf ») : voir Renderer::LoadFont.
        std::optional<UI::FontRef> LoadAssetFont(std::string_view relativePath);
        // Interface du projet (fichier .fluxui, chemin relatif au dossier des assets), relue quand le fichier change :
        // l'éditeur d'interface enregistre, le jeu se met à jour. nullptr sans projet, ou si le fichier est absent ou
        // illisible (l'erreur va dans la console).
        const Interface::Document* LoadInterface(std::string_view relativePath);
        // Réglages de ScreenUi::Draw pour la scène : images, polices et interfaces du projet. Pendant le jeu, les interfaces
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
        // Liaisons des interfaces (champ « Liaison » d'une case, d'un curseur, d'une liste déroulante, d'une barre de
        // progression ou d'un texte) : une valeur nommée, lue à chaque image pour l'afficher, et changée par set quand
        // le joueur touche la case (0 ou 1), le curseur ou la liste (numéro du choix ; sans set : en lecture seule). Le moteur fournit les siennes (« V-Sync »,
        // « Volume général », « Images par seconde ») ; celles du jeu remplacent une valeur du même nom et sont
        // retirées avec ses fonctions quand le module de jeu est déchargé.
        //     engine.BindInterfaceValue("Vie", [] { return g_vie; });
        //     engine.BindInterfaceValue("Sensibilité", [] { return g_sensibilite; }, [](double v) { g_sensibilite = v; });
        //     engine.BindInterfaceText("Nom du vaisseau", [] { return g_nom; });
        void BindInterfaceValue(std::string name, std::function<double()> get, std::function<void(double)> set = {});
        void BindInterfaceText(std::string name, std::function<std::string()> get);
        // Valeur à choix (numéro, 0 le premier) : une liste déroulante liée montre ces choix-là.
        //     engine.BindInterfaceChoice("Difficulté", {"Facile", "Normale", "Difficile"}, [] { return g_niveau; },
        //                                [](double v) { g_niveau = static_cast<int>(v); });
        void BindInterfaceChoice(std::string name, std::vector<std::string> options, std::function<double()> get,
                                 std::function<void(double)> set);
        void UnbindInterfaceValue(std::string_view name);
        // Valeur liable, pour l'éditeur : son nom, sa catégorie (« Graphismes », « Son », « Informations », « Jeu »
        // pour celles du jeu, « Touches » pour les touches des actions) et ses choix (valeur à choix seulement).
        struct InterfaceValueInfo
        {
            std::string name;
            std::string_view category;
            std::vector<std::string> options;
        };
        // Celles du moteur puis du jeu, puis les touches.
        std::vector<InterfaceValueInfo> GetInterfaceValues() const;
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
        // Sauvegarde rapide (F8) : reprise à la fin de l'image au lancement du jeu, joueur remis sur son siège ; false
        // sans sauvegarde, ou si la scène de démarrage du projet a changé depuis (la scène modifiée dans l'éditeur
        // l'emporte ; F9 la recharge quand même). Effacée par DeleteQuickSave (F7).
        bool ResumeQuickSave();
        void DeleteQuickSave();

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
        // Systèmes du jeu (missions...) : fonctions du module de jeu appelées pendant le jeu à une étape précise de
        // l'image, dans l'ordre d'ajout. Le moteur garde les briques (composants, Missions::SpawnEnemies...), le jeu
        // décide de leur usage. Retirés tous quand le module de jeu est déchargé (leur code est dans sa DLL).
        enum class GameStage {
            Interaction, // avant les sièges, hors pause : peut prendre la touche Interagir (ConsumeInteraction)
            BeforePhysics, // après le vol des vaisseaux, le pilote automatique et le warp : mener des vaisseaux
            AfterPhysics,  // après les projectiles, avant l'animation : tout est à sa place, les tirs partent
            Hud,         // indications à l'écran, après la scène et les infos de vol (jeu en cours, joueur présent)
            Reset,       // scène chargée ou jeu arrêté : oublier ce qui tenait à l'ancienne scène (deltaSeconds = 0)
        };
        // Remplace le système du même nom.
        void AddGameSystem(GameStage stage, std::string name, UpdateFunction function);
        void RemoveGameSystem(std::string_view name);
        // Retire aussi les données de sauvegarde (AddGameSaveData).
        void ClearGameSystems();
        // Sauvegarde rapide : l'état propre d'un système du jeu (mission en cours...), que la scène ne garde pas. save
        // le donne en texte (son format ; vide : rien à garder), load le reçoit au chargement, après la scène et
        // l'étape Reset (ses entités sont là, retrouvées par Scene::FindById). Remplace celles du même nom.
        using SaveFunction = std::function<std::string(Engine& engine)>;
        using LoadFunction = std::function<void(Engine& engine, std::string_view data)>;
        void AddGameSaveData(std::string name, SaveFunction save, LoadFunction load);
        void RemoveGameSaveData(std::string_view name);
        // Personnage du joueur pendant le jeu, NullEntity sinon.
        Entity GetPlayer() const { return GetFollowedPlayer(); }
        // Projectiles en vol : ceux que le jeu y ajoute (Weapons::Update, CombatAI::Fire) avancent, touchent et sont
        // dessinés par le moteur.
        std::vector<Projectile>& GetProjectiles() { return m_projectiles; }
        // Étape Interaction : vrai si la touche Interagir a été enfoncée à cette image et que personne ne l'a prise ;
        // elle est alors prise (les sièges ne la voient plus).
        bool ConsumeInteraction();
        // Objectif du jeu (une mission) proposé comme destination du warp, sous ce nom ; nullopt le retire. Effacé
        // avec la scène.
        void SetWarpObjective(std::string name, std::optional<Math::WorldPosition> position);
        // Lignes du jeu (armes actives, missiles...) ajoutées sous les infos de vol du vaisseau piloté ; vide : aucune.
        // Gardées jusqu'au prochain appel, effacées avec la scène.
        void SetFlightInfoExtra(std::string_view text);
        // Arrêter le jeu efface aussi son monde physique : il sera recréé d'après la scène au prochain lancement.
        void SetGameState(GameState state);
        GameState GetGameState() const { return m_gameState; }
        // Écran de chargement affiché (EngineConfig::loadingScreen) : le jeu (physique, module du jeu) attend.
        bool IsLoading() const;
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
        struct GameSystem
        {
            GameStage stage;
            std::string name;
            UpdateFunction function;
        };
        std::vector<GameSystem> m_gameSystems;
        bool m_interactionPending = false; // Interagir enfoncée à cette image, pas encore prise
        std::string m_warpObjectiveName;
        std::optional<Math::WorldPosition> m_warpObjective;
        GameState m_gameState = GameState::Running;
        UpdateFunction m_editorUpdate;
        CloseFunction m_closeRequestHandler;
        // Provisoire : la caméra et son pilotage iront dans le viewport de l'éditeur.
        Camera m_camera;
        FlyCameraController m_cameraController;
        bool m_quitRequested = false;
        bool m_showDebugOverlay = true;
        bool m_drawScreenUi = true;
        bool m_freeCamera = true;
        std::optional<std::string> m_requestedScene;
        std::shared_ptr<PhysicsContext> m_physicsContext;
        std::unique_ptr<PhysicsWorld> m_physics;
        std::unique_ptr<AudioSystem> m_audio;
        std::unique_ptr<InterfaceLibrary> m_interfaces;
        std::unique_ptr<InputActions> m_inputActions;
        // Services privés du moteur (src/Core) : leur détail ne recompile ni l'éditeur ni le jeu.
        std::unique_ptr<InterfaceBindings> m_interfaceBindings; // valeurs, fonctions et actions des interfaces
        std::unique_ptr<PlayerCamera> m_playerCamera;           // caméra qui suit le joueur, cockpit, vue extérieure
        std::unique_ptr<FlightHud> m_flightHud;                 // infos de vol et message en haut de l'écran
        std::unique_ptr<LoadingScreen> m_loadingScreen;
        std::unique_ptr<QuickSave> m_quickSave;                 // F7 / F8 / F9
        std::uint64_t m_frame = 0;
        bool m_interfaceWantsMouse = false; // à l'image précédente
        bool m_interfaceMouseNow = false;   // à l'image en cours, d'après les interfaces déjà dessinées
        bool m_cameraInputEnabled = true;
        bool m_interfacePaused = false;    // à l'image précédente
        bool m_interfacePausedNow = false; // à l'image en cours
        bool m_escapeUsed = false;         // Échap a déjà servi à une interface dans cette image
        double m_fps = 0.0; // moyenne de la dernière seconde (« Images par seconde »)
        std::string m_rebindingAction; // action dont on attend la nouvelle touche
        bool m_keyCaptured = false;    // une touche a servi à la réassignation à cette image : le jeu ne la voit pas
        void CaptureKeyRebinding();
        void SaveInputActions() const;
        InterfaceFunction m_gameQuitHandler;
        // Projectiles en vol (Weapons), vidés quand le jeu s'arrête ou qu'une scène se charge.
        std::vector<Projectile> m_projectiles;
        std::vector<ProjectileHit> m_projectileHits; // de l'image, gardés d'une image à l'autre : pas d'allocation
        std::vector<Impact> m_impacts;
        std::vector<std::string> m_destroyedNames;
        // Warp (EngineWarp.cpp) : destinations du vaisseau piloté (recalculées à chaque image, mémoire gardée), celle
        // que vise son nez (-1 : aucune) et le nom de celle vers laquelle il file (vide hors warp).
        std::vector<Warp::Destination> m_warpDestinations;
        int m_warpAim = -1;
        std::string m_warpName;

        // Composants du moteur qui agissent pendant le jeu (changement de scène), avant la mise à jour du jeu.
        void UpdateGameSystems(double deltaSeconds);
        void LoadRequestedScene();
        // Sauvegarde et chargement rapides demandés pendant l'image (F8 / F9), faits à la fin.
        void RunQuickSave();
        // Recharge la sauvegarde rapide, joueur remis sur son siège s'il y était.
        void QuickLoad(bool resumes);
        // Nouveau monde physique pour la scène actuelle, corps déjà créés.
        void StartPhysics();
        // Personnage suivi par la caméra (celui du joueur), NullEntity sans jeu en cours ou sans personnage.
        Entity GetFollowedPlayer() const;
        void UpdateWarp(Entity ship, bool toggle);
        void DrawWarpInfo(Entity player, Math::Vector2f screen);
        std::string GetDestinationName(const Warp::Destination& destination) const;
        void RunGameSystems(GameStage stage, double deltaSeconds);
        // Scène chargée ou jeu arrêté : destinations du warp et objectif oubliés, systèmes du jeu prévenus.
        void ResetGameState();
        std::optional<std::pair<Math::Vector2f, Math::Vector2f>> DrawInterface(const ScreenUi::InterfaceRequest& request);
    };
}
