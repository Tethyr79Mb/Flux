#pragma once

#include <Flux/Core/Project.hpp>
#include <Flux/Core/Simulation.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace Game {
    struct SimState;

    // Une zone du jeu simulée sans rendu : projet, scène, jeu simulé (GameSim) et cadence de 60 pas par seconde.
    // Pas encore de joueur ni de réseau : le transport (étape 5) et la réplication (étape 6) s'y brancheront.
    class GameServer {
        public:
        GameServer();
        ~GameServer();

        GameServer(const GameServer&) = delete;
        GameServer& operator=(const GameServer&) = delete;

        // scene : relative au dossier des assets, vide pour la scène de démarrage du projet. Lance la simulation.
        std::expected<void, std::string> Open(Flux::Project project, const std::filesystem::path& scene);
        // count pas d'affilée, sans attendre le temps réel (tests, CI).
        void RunSteps(std::uint64_t count);
        // Au rythme du temps réel jusqu'à ce que shouldStop rende vrai ; un bilan dans le journal toutes les 10 s.
        void Run(const std::function<bool()>& shouldStop);

        Flux::Simulation& GetSimulation() { return m_simulation; }

        private:
        Flux::Simulation m_simulation;
        std::shared_ptr<SimState> m_state;
    };
}
