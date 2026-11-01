#pragma once

#include <Flux/Math/WorldPosition.hpp>
#include <Flux/Scene/Components.hpp>
#include <Flux/Scene/SceneSerializer.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace Flux {
    class ComponentRegistry;
    class Scene;
}

// Composant et briques des missions du jeu (sortis du moteur le 2026-11-01).
namespace Game {
    // Donneur de missions (un personnage non joueur) : à moins de reach mètres, F ouvre sa fenêtre de mission. Une
    // seule sorte de mission pour l'instant : détruire un groupe d'ennemis qui apparaît à distance mètres devant la
    // station où il se trouve (le -z de son vaisseau ; sans vaisseau, le sien), répartis sur un cercle de spread mètres.
    // Chaque ennemi est un exemplaire de enemyPrefab (un vaisseau qui n'a pas de points de vie en reçoit) ; sans prefab,
    // un cube rouge à abattre.
    struct MissionGiverComponent
    {
        std::string title = "Chasse aux pirates";
        std::string briefing = "Des pirates rôdent près de la station. Détruisez tout le groupe.";
        std::string enemyPrefab;     // relatif aux assets (.prefab.json)
        std::int32_t enemyCount = 3;
        float distance = 500000.0f;  // m, de la station au centre du groupe : loin, on y va en warp
        float spread = 60.0f;        // m, rayon du cercle des ennemis
        float reach = 2.5f;          // m, du joueur au donneur pour lui parler
    };

    // Enregistre MissionGiverComponent (« missionGiver », même nom qu'au temps du moteur : les scènes se relisent).
    void RegisterMissionGiver(Flux::ComponentRegistry& registry);

    // Missions données par un PNJ (MissionGiverComponent). Une seule sorte pour l'instant : détruire un groupe
    // d'ennemis. Le déroulement (mission en cours, fenêtre) est dans Missions.cpp ; ces fonctions font le travail sur la scène.
    namespace Missions {
        // Donneur de missions à portée du personnage (le plus proche), NullEntity sinon.
        Flux::Entity FindGiver(const Flux::Scene& scene, Flux::Entity player);

        // Centre de la zone où apparaissent les ennemis de la mission de ce donneur.
        Flux::Math::WorldPosition GetTargetArea(const Flux::Scene& scene, Flux::Entity giver);

        // Fait apparaître le groupe d'ennemis, à la racine de la scène, tournés vers la station ; rend leurs racines.
        // Un prefab illisible : rien n'apparaît, et le message d'erreur.
        std::expected<std::vector<Flux::Entity>, std::string> SpawnEnemies(Flux::Scene& scene, const Flux::ComponentRegistry& components,
                                                                            const Flux::SceneSerializer::PrefabReader& prefabs,
                                                                     Flux::Entity giver);

        // Ennemis encore là (pas détruits).
        int CountAlive(const Flux::Scene& scene, std::span<const Flux::Entity> enemies);
    }
}
