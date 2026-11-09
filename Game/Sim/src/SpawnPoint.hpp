#pragma once

#include <Flux/Scene/Components.hpp>

#include <expected>
#include <string>
#include <string_view>

namespace Flux {
    class ComponentRegistry;
    class Scene;
    class Simulation;
}

// Arrivée des joueurs dans une zone du serveur : où leur personnage apparaît, et de quel prefab il est fait.
namespace Game {
    // Point d'apparition : un joueur qui rejoint la zone y reçoit un exemplaire de prefab (son personnage), placé et
    // orienté comme ce point, dans le vaisseau ou la station qui le porte. Provisoire (Bruno, 2026-11-09) : dans le
    // hangar, devant la console, en attendant l'histoire du jeu.
    struct SpawnPointComponent
    {
        std::string prefab; // relatif aux assets (.prefab.json) : un personnage (CharacterComponent)
    };

    void RegisterSpawnPoint(Flux::ComponentRegistry& registry);

    namespace SpawnPoints {
        // Personnages joueurs posés dans la scène pour le solo : retirés par le serveur, où seuls les joueurs
        // connectés en ont un. Rend leur nombre.
        int RemovePlacedPlayers(Flux::Scene& scene);
        // Crée le personnage d'un joueur au premier point d'apparition : prefab demandé s'il est celui d'un point
        // d'apparition (vide : celui du point), personnage joué (CharacterComponent::player), nommé d'après lui.
        std::expected<Flux::Entity, std::string> Spawn(Flux::Simulation& simulation, std::string_view prefab,
                                                       std::string_view playerName);
    }
}
