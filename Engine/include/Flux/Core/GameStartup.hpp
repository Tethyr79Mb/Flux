#pragma once

#include "Flux/Core/GameSettings.hpp"
#include "Flux/Export.hpp"

#include <filesystem>

namespace Flux {
    class Engine;

    // Démarrage du jeu sans éditeur, pour le main de Game.exe (et plus tard du lanceur) : lit Game.fluxproject et les
    // réglages du joueur (Reglages.json) à côté de l'exe, crée le moteur avec ces réglages (fenêtre, V-Sync, volume),
    // charge le module du jeu (moduleFile, à côté de l'exe aussi), ouvre la scène de démarrage et fait tourner la
    // boucle. À la fermeture, les réglages changés en jeu (interface Paramètres, taille de la fenêtre) sont réécrits.
    class FLUX_API GameStartup {
        public:
        explicit GameStartup(std::filesystem::path moduleFile);

        // Code de sortie du programme : 1 si le projet, le module ou la scène ne se chargent pas.
        int Run();

        // Les réglages tels que le moteur les a laissés (fenêtre, V-Sync, volume) ; le reste ne change pas.
        static GameSettings ReadBack(Engine& engine, GameSettings settings);

        private:
        std::filesystem::path m_moduleFile;
    };
}
