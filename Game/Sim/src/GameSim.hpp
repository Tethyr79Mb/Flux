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
    // État de jeu d'un joueur, sur son personnage (comme PilotWeaponsComponent) : chacun ses missions, sa navigation et
    // ses messages. Le temps du jeu seulement (ni inspecteur ni sauvegarde de scène).
    struct PlayerStateComponent
    {
        MissionState missions;
        NavigationState navigation;
        // Messages brefs (« Warp coupé »), dans l'ordre : vidés par l'affichage du client ; les plus anciens perdus
        // au-delà de MaxNotices (joueur d'un serveur, que rien n'affiche encore).
        std::vector<std::string> notices;
        static constexpr std::size_t MaxNotices = 16;
    };

    // État de jeu du joueur, créé au premier besoin.
    PlayerStateComponent& GetPlayerState(Flux::Scene& scene, Flux::Entity player);
    // nullptr s'il n'en a pas (encore) ou n'existe plus.
    PlayerStateComponent* FindPlayerState(Flux::Scene& scene, Flux::Entity player);
    void AddNotice(PlayerStateComponent& state, std::string notice);

    // État du jeu propre à une simulation (une par zone sur un serveur), que la scène ne garde pas. Partagé par les
    // systèmes enregistrés sur elle et par l'affichage du client ; détruit avec le dernier d'entre eux.
    struct SimState
    {
        PilotActions pilot;
        NavigationActions navigation;
        PlayerWeaponsState weapons;
        std::vector<std::string> destroyed; // noms des cibles détruites au pas, gardés d'un pas à l'autre
        std::vector<Flux::Entity> combatTargets; // vaisseaux des joueurs visés par les ennemis, gardés aussi
        float spinSpeed = 1.0f;             // multiplie la vitesse des rotations continues (démo)
    };

    // Composants du jeu, ses actions (même bits sur le client et le serveur) et ses systèmes ; rend l'état partagé.
    std::shared_ptr<SimState> RegisterSimulation(Flux::Simulation& simulation);
    // Retire les systèmes (le code du module de jeu va être déchargé). Les composants restent à vider par l'hôte.
    void UnregisterSimulation(Flux::Simulation& simulation);
}
