#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/WorldPosition.hpp"
#include "Flux/Scene/Components.hpp"
#include "Flux/Scene/SceneSerializer.hpp"

#include <expected>
#include <span>
#include <string>
#include <vector>

namespace Flux {
    class ComponentRegistry;
    class Scene;

    // Missions données par un PNJ (MissionGiverComponent). Une seule sorte pour l'instant : détruire un groupe
    // d'ennemis. Le moteur garde la mission en cours (Engine) ; ces fonctions font le travail sur la scène.
    namespace Missions {
        // Donneur de missions à portée du personnage (le plus proche), NullEntity sinon.
        FLUX_API Entity FindGiver(const Scene& scene, Entity player);

        // Centre de la zone où apparaissent les ennemis de la mission de ce donneur.
        FLUX_API Math::WorldPosition GetTargetArea(const Scene& scene, Entity giver);

        // Fait apparaître le groupe d'ennemis, à la racine de la scène, tournés vers la station ; rend leurs racines.
        // Un prefab illisible : rien n'apparaît, et le message d'erreur.
        FLUX_API std::expected<std::vector<Entity>, std::string> SpawnEnemies(Scene& scene, const ComponentRegistry& components,
                                                                            const SceneSerializer::PrefabReader& prefabs,
                                                                            Entity giver);

        // Ennemis encore là (pas détruits).
        FLUX_API int CountAlive(const Scene& scene, std::span<const Entity> enemies);
    }
}
