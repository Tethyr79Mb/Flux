// Missions : un PNJ de la station (donneur) envoie détruire un groupe d'ennemis, loin devant la station.
#include "Check.hpp"

#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Missions.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"

#include <cmath>
#include <map>
#include <numbers>
#include <string>

using namespace Flux;

namespace {
    double Distance(const Math::WorldPosition& a, const Math::WorldPosition& b) { return Math::Length(Math::Delta(a, b)); }
}

bool RunMissionTests() {
    ComponentRegistry components;
    RegisterEngineComponents(components);

    // Station tournée d'un quart de tour : son avant (-z) pointe vers -x du monde.
    Scene scene;
    const Entity station = scene.CreateEntity("Station");
    TransformComponent& stationTransform = scene.Get<TransformComponent>(station);
    stationTransform.position = {1000.0, 0.0, 0.0};
    stationTransform.rotation = Math::Quaternionf::FromAxisAngle({0.0f, 1.0f, 0.0f}, 0.5f * std::numbers::pi_v<float>);
    scene.Add<ShipComponent>(station, ShipComponent{.maxSpeed = 0.0f, .fallsWithoutPilot = false});
    const Entity giver = scene.CreateEntity("Marchand", station);
    scene.Get<TransformComponent>(giver).position = {0.0, 0.0, 60.0};
    scene.Add<MissionGiverComponent>(giver, MissionGiverComponent{.enemyCount = 3, .distance = 5000.0f, .spread = 50.0f});

    // Le joueur parle au donneur seulement à portée.
    const Entity player = scene.CreateEntity("Joueur", station);
    scene.Get<TransformComponent>(player).position = {0.0, 0.0, 58.5};
    CHECK(Missions::FindGiver(scene, player) == giver);
    scene.Get<TransformComponent>(player).position = {0.0, 0.0, 50.0};
    CHECK(Missions::FindGiver(scene, player) == NullEntity);

    // Zone de la mission : à `distance` devant la station (pas devant le PNJ).
    const Math::WorldPosition area = Missions::GetTargetArea(scene, giver);
    CHECK(Distance(area, Math::WorldPosition{{}, {-4000.0, 0.0, 0.0}}) < 0.1);

    // Sans prefab : des drones (vaisseaux cubes armés, menés par l'IA de combat), destructibles, en cercle autour de
    // la zone, à la racine.
    const ComponentRegistry& registry = components;
    const SceneSerializer::PrefabReader none = [](std::string_view) -> std::expected<std::string, std::string> {
        return std::unexpected("absent");
    };
    const auto cubes = Missions::SpawnEnemies(scene, registry, none, giver);
    CHECK(cubes.has_value() && cubes->size() == 3);
    for (const Entity enemy : *cubes) {
        CHECK(scene.GetParent(enemy) == NullEntity);
        CHECK(scene.TryGet<HealthComponent>(enemy) && scene.TryGet<ShipComponent>(enemy) && scene.TryGet<CombatAIComponent>(enemy));
        bool armed = false;
        scene.ForEachChild(enemy, [&](Entity child) { armed = armed || scene.TryGet<HardpointComponent>(child); });
        CHECK(armed);
        CHECK(std::abs(Distance(scene.GetWorldPosition(enemy), area) - 50.0) < 0.1);
    }
    CHECK(Missions::CountAlive(scene, *cubes) == 3);
    scene.DestroyEntity((*cubes)[1]);
    CHECK(Missions::CountAlive(scene, *cubes) == 2);

    // Prefab illisible : erreur, et aucun ennemi ne reste dans la scène.
    const std::size_t before = scene.GetEntityCount();
    scene.Get<MissionGiverComponent>(giver).enemyPrefab = "Prefabs/Pirate.prefab.json";
    CHECK(!Missions::SpawnEnemies(scene, registry, none, giver).has_value());
    CHECK(scene.GetEntityCount() == before);

    // Prefab d'un vaisseau sans points de vie : il en reçoit, et attend en place au lieu de tomber.
    Scene source;
    const Entity pirate = source.CreateEntity("Pirate");
    source.Add<ShipComponent>(pirate);
    const Entity hull = source.CreateEntity("Coque", pirate);
    source.Add<ColliderComponent>(hull);
    const auto prefab = SceneSerializer::SavePrefab(source, components, pirate, SceneSerializer::PrefabIds::ForInstance);
    CHECK(prefab.has_value());
    std::map<std::string, std::string, std::less<>> files{{"Prefabs/Pirate.prefab.json", *prefab}};
    const SceneSerializer::PrefabReader reader = [&](std::string_view file) -> std::expected<std::string, std::string> {
        const auto found = files.find(file);
        return found != files.end() ? std::expected<std::string, std::string>(found->second) : std::unexpected("absent");
    };
    scene.Get<MissionGiverComponent>(giver).enemyCount = 1;
    const auto ships = Missions::SpawnEnemies(scene, registry, reader, giver);
    CHECK(ships.has_value() && ships->size() == 1);
    const Entity ship = ships->front();
    CHECK(scene.TryGet<HealthComponent>(ship) != nullptr);
    CHECK(!scene.Get<ShipComponent>(ship).fallsWithoutPilot);
    CHECK(scene.TryGet<CombatAIComponent>(ship) != nullptr); // il se défendra
    CHECK(Distance(scene.GetWorldPosition(ship), area) < 0.1); // seul : au centre
    return true;
}
