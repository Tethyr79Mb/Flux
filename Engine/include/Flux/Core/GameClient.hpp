#pragma once

#include "Flux/Export.hpp"

#include <filesystem>

namespace Flux {
    // Démarrage complet du jeu sans éditeur, pour le main de Game.exe (et plus tard du lanceur) : lit Game.fluxproject à
    // côté de l'exe, crée le moteur, charge le module du jeu (moduleFile, à côté de l'exe aussi), ouvre la scène de
    // démarrage et fait tourner la boucle jusqu'à la fermeture. Le moteur garde la boucle, comme dans l'éditeur.
    // Renvoie le code de sortie du programme : 1 si le projet, le module ou la scène ne se chargent pas.
    FLUX_API int RunGame(const std::filesystem::path& moduleFile);
}
