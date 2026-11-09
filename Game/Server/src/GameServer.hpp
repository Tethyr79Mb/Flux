#pragma once

#include <Flux/Core/ByteStream.hpp>
#include <Flux/Core/Project.hpp>
#include <Flux/Core/Simulation.hpp>
#include <Flux/Net/Server.hpp>
#include <Flux/Scene/Replication.hpp>

#include <bitset>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Game {
    struct SimState;

    // Une zone du jeu simulée sans rendu : projet, scène, jeu simulé (GameSim) et cadence de 60 pas par seconde.
    // Accueille les clients (FluxNet) et leur envoie le monde 30 fois par seconde (Replication.hpp). Chaque client
    // qui rejoint (Join) reçoit un personnage au point d'apparition, mené par ses commandes, retiré à son départ.
    class GameServer {
        public:
        GameServer();
        ~GameServer();

        GameServer(const GameServer&) = delete;
        GameServer& operator=(const GameServer&) = delete;

        // scene : relative au dossier des assets, vide pour la scène de démarrage du projet. Lance la simulation.
        std::expected<void, std::string> Open(Flux::Project project, const std::filesystem::path& scene);
        // Accueille les clients sur ce port UDP (versions vérifiées à la connexion), servis pendant Run.
        std::expected<void, std::string> Listen(std::uint16_t port);
        // count pas d'affilée, sans attendre le temps réel (tests, CI) : commandes des joueurs, puis le pas.
        void RunSteps(std::uint64_t count);
        // Au rythme du temps réel jusqu'à ce que shouldStop rende vrai ; un bilan dans le journal toutes les 10 s.
        void Run(const std::function<bool()>& shouldStop);

        Flux::Simulation& GetSimulation() { return m_simulation; }
        // Relève le réseau (connexions, accusés de réception), puis envoie un instantané du monde à chaque client.
        // Appelé par Run tous les SnapshotIntervalSteps pas ; à la main par les tests.
        void ServeNetwork();
        void Replicate();

        private:
        struct Client
        {
            std::uint32_t acknowledged = 0; // dernier instantané reçu, base des différences ; 0 : aucun
            std::unordered_set<Flux::Uuid> spawned; // entités créées pendant le jeu déjà envoyées (EntitySpawn)
            std::string name;                         // --joueur du client (Join)
            Flux::Entity player = Flux::NullEntity;   // son personnage
            // Sa dernière commande, et les actions enfoncées depuis le dernier pas (aucune perdue entre deux pas).
            Flux::PlayerCommand command;
            std::bitset<Flux::MaxInputActions> pressed;
            std::uint64_t lastCommandStep = 0; // pas du client de la plus récente reçue
        };

        void SendWelcome(Flux::Net::ConnectionId connection);
        void Join(Flux::Net::ConnectionId connection, Client& client, Flux::ByteReader& reader);
        void ReadCommands(Client& client, Flux::ByteReader& reader);
        void Leave(Client& client);
        // Avant chaque pas : la commande de chaque joueur, ses actions enfoncées une seule fois.
        void ApplyCommands();
        void Step();
        void SendSpawns(Flux::Net::ConnectionId connection, Client& client, const Flux::Replication::WorldState& state);

        Flux::Simulation m_simulation;
        Flux::Net::Server m_network;
        std::shared_ptr<SimState> m_state;
        std::string m_scene; // relative au dossier des assets, envoyée aux clients
        // Entités de la scène lue : les clients les ont dans leur fichier. Les autres, créées pendant le jeu, leur sont
        // envoyées en entier une fois (EntitySpawn).
        std::unordered_set<Flux::Uuid> m_sceneEntities;
        Flux::Replication::Schema m_schema;
        Flux::Replication::Capturer m_capturer;
        Flux::Replication::SnapshotHistory m_history;
        std::unordered_map<Flux::Net::ConnectionId, Client> m_clients;
        Flux::ByteWriter m_message; // gardé d'un envoi à l'autre : pas d'allocation par instantané
        std::uint64_t m_lastSnapshotStep = 0;
    };
}
