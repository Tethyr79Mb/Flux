#pragma once

#include <Flux/Core/Project.hpp>
#include <Flux/Core/Simulation.hpp>
#include <Flux/Net/Server.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace Game {
    struct SimState;

    // Une zone du jeu simulée sans rendu : projet, scène, jeu simulé (GameSim) et cadence de 60 pas par seconde.
    // Accueille les clients (FluxNet) : connexion et déconnexion seulement, sans joueur dans le monde avant l'étape 6
    // (réplication).
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
        // count pas d'affilée, sans attendre le temps réel (tests, CI).
        void RunSteps(std::uint64_t count);
        // Au rythme du temps réel jusqu'à ce que shouldStop rende vrai ; un bilan dans le journal toutes les 10 s.
        void Run(const std::function<bool()>& shouldStop);

        Flux::Simulation& GetSimulation() { return m_simulation; }

        private:
        void ServeNetwork();

        Flux::Simulation m_simulation;
        Flux::Net::Server m_network;
        std::shared_ptr<SimState> m_state;
    };
}
