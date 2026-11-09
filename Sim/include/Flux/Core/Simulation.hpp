#pragma once

#include "Flux/Core/Project.hpp"
#include "Flux/Core/SimulationClock.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/PlayerCommand.hpp"
#include "Flux/Scene/Projectiles.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"
#include "Flux/Scene/Ships.hpp"
#include "Flux/SimExport.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Flux {
    class FlightSurroundings;
    class PhysicsContext;
    class PhysicsWorld;

    // Actions du personnage, déclarées par la simulation dans cet ordre : les touches du client s'y rattachent par ces
    // noms. Hors de la classe : une constante membre d'une classe exportée serait importée de la DLL.
    namespace CharacterActions {
        inline constexpr std::string_view Forward = "Avancer";
        inline constexpr std::string_view Back = "Reculer";
        inline constexpr std::string_view Left = "Aller à gauche";
        inline constexpr std::string_view Right = "Aller à droite";
        inline constexpr std::string_view Run = "Courir";
        inline constexpr std::string_view Jump = "Sauter";
        inline constexpr std::string_view Interact = "Interagir";
    }

    // Le monde simulé, sans rendu ni fenêtre : scène, composants, physique, projectiles, pas fixe et systèmes du jeu.
    // Le moteur (Engine) en possède une et la fait avancer à chaque image ; le serveur de jeu en fera tourner une seule,
    // sans affichage. Ce qui se voit (caméra, impacts, interfaces, son) reste au moteur.
    class FLUX_SIM_API Simulation {
        public:
        // Système du jeu ou du moteur appelé à chaque pas (deltaSeconds = SimulationClock::StepSeconds).
        using StepFunction = std::function<void(Simulation& simulation, double deltaSeconds)>;
        // Durée (s, vitesse de lecture 1) d'une animation d'un modèle ; 0 si inconnue. Donnée par qui charge les
        // modèles (le rendu pour l'instant).
        using AnimationDurationFunction = std::function<float(const std::filesystem::path& model, const std::string& animation)>;

        // Étapes d'un pas où tournent les systèmes, dans l'ordre d'ajout.
        enum class Stage {
            Interaction,   // avant les sièges : peut prendre la touche Interagir (ConsumeInteraction)
            BeforePhysics, // après le vol des vaisseaux : pilote automatique, warp, IA (mener des vaisseaux)
            AfterPhysics,  // après les projectiles, avant l'animation : tout est à sa place, les tirs partent
            Reset,         // scène remplacée ou simulation arrêtée : oublier l'ancienne scène (deltaSeconds = 0)
            Replication,   // monde répliqué (SetReplicated) : la seule étape d'un pas, où arrive l'état du serveur
        };

        Simulation();
        ~Simulation();

        Simulation(const Simulation&) = delete;
        Simulation& operator=(const Simulation&) = delete;

        Scene& GetScene() { return m_scene; }
        const Scene& GetScene() const { return m_scene; }
        // Composants connus (ceux du moteur, puis ceux du jeu) : lecture et écriture des scènes.
        ComponentRegistry& GetComponents() { return m_components; }

        // Projet ouvert : dossier des assets (modèles de collision, scènes) et préfabriqués.
        void SetProject(Project project);
        const Project* GetProject() const { return m_project ? &*m_project : nullptr; }
        Project* GetProject() { return m_project ? &*m_project : nullptr; }
        const SceneSerializer::PrefabReader& GetPrefabReader() const { return m_prefabReader; }

        // Lit la scène du fichier à la place de l'actuelle, puis SceneReplaced. En cas d'échec, la scène reste.
        std::expected<void, std::string> LoadScene(const std::filesystem::path& path);
        // La scène vient d'être remplacée (chargement, fusion d'une sauvegarde) : monde physique, projectiles,
        // horloge et état des systèmes oubliés ; un nouveau monde physique est préparé si la simulation tourne.
        void SceneReplaced();

        // Lancement et arrêt du jeu : le monde physique est créé d'après la scène, et effacé à l'arrêt (projectiles,
        // horloge et état des systèmes aussi).
        void Start();
        void Stop();
        bool IsStarted() const { return m_started; }

        // Un pas de simulation : sièges, vol des vaisseaux, physique, projectiles, animation et systèmes du jeu.
        // Rien de tout cela quand le monde est répliqué.
        void Step(double deltaSeconds);
        // Monde simulé ailleurs (client d'un serveur de jeu) : Step ne simule plus rien et ne fait tourner que l'étape
        // Replication, où la scène reçoit l'état du serveur (Replication.hpp). L'horloge avance toujours, à qui fait
        // tourner.
        void SetReplicated(bool replicated) { m_replicated = replicated; }
        bool IsReplicated() const { return m_replicated; }
        // Pas : leur nombre depuis le lancement, et la part du suivant déjà écoulée (avancée par qui fait tourner).
        SimulationClock& GetClock() { return m_clock; }
        const SimulationClock& GetClock() const { return m_clock; }

        // Monde physique, nullptr à l'arrêt. Tous partagent le contexte physique de la simulation.
        PhysicsWorld* GetPhysics() { return m_physics.get(); }
        // Personnage du joueur (celui que suit la caméra), NullEntity à l'arrêt ou sans personnage.
        Entity GetPlayer() const;
        // Commande du joueur qui dirige ce personnage pour le pas en cours ; vide pour un personnage sans joueur.
        // Pour l'instant, seul le joueur de cet écran en a une, et il l'a aussi sans personnage (vol libre).
        const PlayerCommand& GetPlayerCommand(Entity player) const;
        // Actions que lit la simulation, par leur bit dans PlayerCommand : celles du personnage, puis celles du jeu.
        CommandActions& GetActions() { return m_actions; }
        const CommandActions& GetActions() const { return m_actions; }
        // Commande du joueur de cet écran pour le prochain pas.
        void SetLocalCommand(const PlayerCommand& command) { m_localCommand = command; }
        void SetAnimationDurations(AnimationDurationFunction durations) { m_animationDurations = std::move(durations); }

        // Projectiles en vol : ceux que le jeu y ajoute (ses armes) avancent et touchent ici, sont dessinés par le
        // moteur.
        std::vector<Projectile>& GetProjectiles() { return m_projectiles; }
        const std::vector<Projectile>& GetProjectiles() const { return m_projectiles; }
        // Projectiles arrivés sur un corps pendant le pas : au jeu d'en retirer les dégâts, à l'étape AfterPhysics.
        std::span<const ProjectileHit> GetProjectileHits() const { return m_projectileHits; }
        // Étape Interaction : vrai si la touche Interagir a été enfoncée pour ce pas et que personne ne l'a prise ;
        // elle est alors prise (les sièges ne la voient plus).
        bool ConsumeInteraction();
        // Commandes du vaisseau piloté pour ce pas (poussée, roulis, frein, boost, mode voyage), données par le jeu à
        // l'étape Interaction. La simulation y ajoute le manche et ce qui entoure le vaisseau (gravité, air,
        // planète), puis le fait voler ; sans appel, le vaisseau ne reçoit aucune commande.
        void SetPilotInput(const ShipInput& input) { m_pilotInput = input; }
        // Planète la plus proche du vaisseau, altitude, air. Recrée le relief seulement quand la planète change.
        FlightState GetFlightState(Entity ship);

        // Remplace le système du même nom.
        void AddSystem(Stage stage, std::string name, StepFunction function);
        void RemoveSystem(std::string_view name);
        void ClearSystems();

        private:
        ComponentRegistry m_components;
        std::optional<Project> m_project;
        SceneSerializer::PrefabReader m_prefabReader;
        Scene m_scene;
        std::shared_ptr<PhysicsContext> m_physicsContext;
        std::unique_ptr<PhysicsWorld> m_physics;
        std::unique_ptr<FlightSurroundings> m_flightSurroundings; // planète, altitude et air autour du vaisseau piloté
        SimulationClock m_clock;
        bool m_started = false;
        bool m_replicated = false;
        struct System
        {
            Stage stage;
            std::string name;
            StepFunction function;
        };
        std::vector<System> m_systems;
        CommandActions m_actions;
        struct CharacterBits
        {
            InputActionId run = NoInputAction;
            InputActionId jump = NoInputAction;
            InputActionId interact = NoInputAction;
        } m_characterActions;
        AnimationDurationFunction m_animationDurations;
        PlayerCommand m_localCommand;
        ShipInput m_pilotInput;            // commandes du jeu pour ce pas (SetPilotInput)
        bool m_interactionPending = false; // Interagir enfoncée pour ce pas, pas encore prise
        std::vector<Projectile> m_projectiles;
        std::vector<ProjectileHit> m_projectileHits; // du pas, gardés d'un pas à l'autre : pas d'allocation

        void StartPhysics();
        void RunSystems(Stage stage, double deltaSeconds);
    };
}
