#include "SpawnPoint.hpp"

#include <Flux/Core/Simulation.hpp>
#include <Flux/Scene/ComponentRegistry.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/SceneSerializer.hpp>

#include <utility>
#include <vector>

namespace Game {
    using namespace Flux;

    void RegisterSpawnPoint(ComponentRegistry& registry) {
        registry.Register<SpawnPointComponent>("spawnPoint", "Point d'apparition")
            .Asset("prefab", "Personnage (prefab)", &SpawnPointComponent::prefab, AssetType::Prefab);
    }

    namespace SpawnPoints {
        int RemovePlacedPlayers(Scene& scene) {
            std::vector<Entity> placed;
            for (const auto [entity, character] : scene.GetRegistry().view<const CharacterComponent>().each()) {
                if (character.player) {
                    placed.push_back(entity);
                }
            }
            for (const Entity entity : placed) {
                if (scene.IsValid(entity)) {
                    scene.DestroyEntity(entity);
                }
            }
            return static_cast<int>(placed.size());
        }

        std::expected<Entity, std::string> Spawn(Simulation& simulation, std::string_view prefab, std::string_view playerName) {
            Scene& scene = simulation.GetScene();
            Entity spawn = NullEntity;
            for (const auto [entity, point] : scene.GetRegistry().view<const SpawnPointComponent>().each()) {
                // Le premier venu, sauf si un autre propose le prefab demandé (un client ne crée que ceux-là).
                if (spawn == NullEntity || (!prefab.empty() && point.prefab == prefab)) {
                    spawn = entity;
                }
            }
            if (spawn == NullEntity) {
                return std::unexpected("aucun point d'apparition dans la scène");
            }
            const SpawnPointComponent& point = scene.Get<SpawnPointComponent>(spawn);
            if (point.prefab.empty()) {
                return std::unexpected("point d'apparition sans prefab de personnage");
            }
            // Sous le parent du point, à sa place ; puis directement sous le vaisseau qui le porte (la physique ne
            // fait marcher que les personnages à la racine ou enfants directs d'un vaisseau).
            const Entity parent = scene.GetParent(spawn);
            auto placed = SceneSerializer::Instantiate(scene, simulation.GetComponents(), point.prefab,
                                                       simulation.GetPrefabReader(), parent);
            if (!placed) {
                return std::unexpected(std::move(placed.error()));
            }
            const Entity character = *placed;
            auto* component = scene.TryGet<CharacterComponent>(character);
            if (!component) {
                scene.DestroyEntity(character);
                return std::unexpected("le prefab " + point.prefab + " n'a pas de personnage");
            }
            component->player = true;
            const TransformComponent& spot = scene.Get<TransformComponent>(spawn);
            TransformComponent& transform = scene.Get<TransformComponent>(character);
            transform.position = spot.position;
            transform.rotation = spot.rotation;
            if (const auto* sector = scene.TryGet<SectorComponent>(spawn)) {
                scene.GetRegistry().emplace_or_replace<SectorComponent>(character, *sector);
            }
            if (const Entity ship = scene.GetShip(spawn); ship != parent) {
                scene.SetParentKeepingWorld(character, ship);
            }
            scene.Get<NameComponent>(character).name = "Joueur : " + std::string(playerName);
            return character;
        }
    }
}
