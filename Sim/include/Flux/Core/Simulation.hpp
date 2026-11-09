#pragma once

#include "Flux/Core/Project.hpp"
#include "Flux/Core/SimulationClock.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/PlayerCommand.hpp"
#include "Flux/Scene/Projectiles.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Scene/SceneSerializer.hpp"
#include "Flux/Scene/Seats.hpp"
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
#include <utility>
#include <vector>

namespace Flux {
    class FlightSurroundings;
    class PhysicsContext;

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
            Replication,   // monde répliqué (SetReplicated) : la seule étape d'un pas, où arrive l'état du serveur et
                           // d'où part la commande de cet écran
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
        // Personnage du joueur de cet écran (celui que suit la caméra), NullEntity à l'arrêt ou sans personnage : le
        // premier personnage joué de la scène, ou dans un monde répliqué celui que le serveur a donné (SetLocalPlayer).
        Entity GetPlayer() const;
        // Monde répliqué : le personnage de ce client, créé par le serveur (NullEntity : aucun pour l'instant).
        void SetLocalPlayer(Entity player) { m_localPlayer = player; }
        // Personnages joués (CharacterComponent::player) pendant le pas en cours : les systèmes du jeu les parcourent.
        std::span<const Entity> GetPlayers() const { return m_players; }
        // Commande du joueur qui dirige ce personnage pour le pas en cours ; vide pour un personnage sans joueur. Le
        // joueur de cet écran a la sienne aussi sans personnage (vol libre).
        const PlayerCommand& GetPlayerCommand(Entity player) const;
        // Actions que lit la simulation, par leur bit dans PlayerCommand : celles du personnage, puis celles du jeu.
        CommandActions& GetActions() { return m_actions; }
        const CommandActions& GetActions() const { return m_actions; }
        // Commande du joueur de cet écran pour le prochain pas.
        void SetLocalCommand(const PlayerCommand& command) { m_localCommand = command; }
        // Commande d'un autre joueur (reçue par le serveur) pour le prochain pas seulement : à redonner à chaque pas.
        void SetPlayerCommand(Entity player, const PlayerCommand& command);
        void SetAnimationDurations(AnimationDurationFunction durations) { m_animationDurations = std::move(durations); }

        // Projectiles en vol : ceux que le jeu y ajoute (ses armes) avancent et touchent ici, sont dessinés par le
        // moteur.
        std::vector<Projectile>& GetProjectiles() { return m_projectiles; }
        const std::vector<Projectile>& GetProjectiles() const { return m_projectiles; }
        // Projectiles arrivés sur un corps pendant le pas : au jeu d'en retirer les dégâts, à l'étape AfterPhysics.
        std::span<const ProjectileHit> GetProjectileHits() const { return m_projectileHits; }
        // Étape Interaction : vrai si ce joueur a enfoncé Interagir pour ce pas et que personne ne l'a prise ; elle
        // est alors prise (les sièges ne la voient plus).
        bool ConsumeInteraction(Entity player);
        // Commandes du vaisseau que pilote ce joueur pour ce pas (poussée, roulis, frein, boost, mode voyage), données
        // par le jeu à l'étape Interaction. La simulation y ajoute le manche et ce qui entoure le vaisseau (gravité,
        // air, planète), puis le fait voler ; sans appel, le vaisseau ne reçoit aucune commande.
        void SetPilotInput(Entity player, const ShipInput& input);
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
        Entity m_localPlayer = NullEntity; // monde répliqué seulement (SetLocalPlayer)
        std::vector<std::pair<Entity, PlayerCommand>> m_commands; // des autres joueurs, pour le prochain pas
        // Un joueur pendant le pas : sa commande, celles du jeu pour son vaisseau (SetPilotInput), et Interagir
        // enfoncée pas encore prise. Les vecteurs du pas gardent leur mémoire : pas d'allocation.
        struct PlayerStep
        {
            Entity entity = NullEntity;
            PlayerCommand command;
            ShipInput pilotInput;
            bool interactionPending = false;
        };
        std::vector<Entity> m_players;
        std::vector<PlayerStep> m_playerSteps; // dans l'ordre de m_players
        std::vector<std::pair<Entity, CharacterInput>> m_characterInputs;
        std::vector<std::pair<Entity, ShipInput>> m_shipInputs;
        std::vector<Projectile> m_projectiles;
        std::vector<ProjectileHit> m_projectileHits; // du pas, gardés d'un pas à l'autre : pas d'allocation
        // Monde répliqué : mouvement de chaque personnage au pas précédent, trié par entité (AnimateReplicated).
        struct CharacterMotion
        {
            Entity entity = NullEntity;
            Entity parent = NullEntity;
            Math::Vector3d position; // dans le repère du parent : le vaisseau qui avance ne fait pas marcher
            double vertical = 0.0;   // vitesse le long de son haut, m/s
            bool inAir = false;
        };
        std::vector<CharacterMotion> m_characterMotions;
        std::vector<CharacterMotion> m_previousCharacterMotions;

        void StartPhysics();
        void CollectPlayers();
        void AnimateReplicated(double deltaSeconds);
        PlayerStep* FindPlayerStep(Entity player);
        Seats::Durations GetSeatDurations(Entity character) const;
        void RunSystems(Stage stage, double deltaSeconds);
    };
}
