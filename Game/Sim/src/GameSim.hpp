#pragma once

#include "Missions.hpp"
#include "Navigation.hpp"
#include "Piloting.hpp"
#include "PlayerWeapons.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Flux {
    class Simulation;
}

// Le jeu qui se simule, sans rien afficher : composants et systèmes du pas (pilotage, navigation, missions, armes,
// combat). Le serveur de jeu ne charge que lui ; le client (GameModule) y ajoute l'affichage, les touches et l'éditeur.
namespace Game {
    // État du jeu propre à une simulation (une par zone sur un serveur), que la scène ne garde pas. Partagé par les
    // systèmes enregistrés sur elle et par l'affichage du client ; détruit avec le dernier d'entre eux.
    struct SimState
    {
        PilotActions pilot;
        NavigationState navigation;
        MissionState missions;
        PlayerWeaponsState weapons;
        std::vector<std::string> destroyed; // noms des cibles détruites au pas, gardés d'un pas à l'autre
        float spinSpeed = 1.0f;             // multiplie la vitesse des rotations continues (démo)
        // Messages brefs pour le joueur (« Warp coupé »), dans l'ordre : vidés par l'affichage du client.
        std::vector<std::string> notices;
    };

    // Composants du jeu, ses actions (même bits sur le client et le serveur) et ses systèmes ; rend l'état partagé.
    std::shared_ptr<SimState> RegisterSimulation(Flux::Simulation& simulation);
    // Retire les systèmes (le code du module de jeu va être déchargé). Les composants restent à vider par l'hôte.
    void UnregisterSimulation(Flux::Simulation& simulation);
}
