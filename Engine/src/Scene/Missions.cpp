#include "Flux/Scene/Missions.hpp"

#include "Flux/Scene/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Flux::Missions {
    namespace {
        constexpr float DefaultHealth = 100.0f; // ennemi sans points de vie : 10 obus du Gatling
        constexpr float DroneSize = 6.0f;       // m, drone cube quand le donneur n'a pas de prefab

        // Ennemi par défaut, sans prefab : un drone cube rouge qui vole et tire (un canon devant), mené par l'IA de
        // combat.
        Entity CreateDrone(Scene& scene, int number) {
            const Entity drone = scene.CreateEntity("Ennemi " + std::to_string(number));
            ShipComponent ship;
            ship.maxSpeed = 120.0f;
            ship.acceleration = 30.0f;
            ship.turnRate = 70.0f;
            ship.levelAssist = false;
            ship.fallsWithoutPilot = false;
            ship.warpSpeed = 0.0f;
            scene.Add<ShipComponent>(drone, ship);
            const Entity hull = scene.CreateEntity("Coque", drone);
            scene.Get<TransformComponent>(hull).scale = {DroneSize, DroneSize, DroneSize};
            scene.Add<MeshRendererComponent>(hull, MeshType::Cube, Math::LinearColor::FromSrgb8(200, 40, 30));
            scene.Add<ColliderComponent>(hull);
            const Entity hardpoint = scene.CreateEntity("Emport", drone);
            scene.Get<TransformComponent>(hardpoint).position = {0.0, 0.0, -DroneSize * 0.5 - 0.1};
            scene.Add<HardpointComponent>(hardpoint);
            const Entity gun = scene.CreateEntity("Canon", hardpoint);
            WeaponComponent weapon;
            weapon.fireRate = 4.0f;
            weapon.damage = 4.0f;
            weapon.range = 2000.0f;
            weapon.cone = 15.0f;
            weapon.muzzle = 1.5f;
            weapon.color = {1.0f, 0.2f, 0.15f};
            scene.Add<WeaponComponent>(gun, weapon);
            const Entity barrel = scene.CreateEntity("Tube", gun);
            scene.Get<TransformComponent>(barrel).position = {0.0, 0.0, -0.75};
            scene.Get<TransformComponent>(barrel).scale = {0.4f, 0.4f, 1.5f};
            scene.Add<MeshRendererComponent>(barrel, MeshType::Cube, Math::LinearColor::FromSrgb8(60, 60, 60));
            scene.Add<CombatAIComponent>(drone);
            return drone;
        }

        // Repère de la station du donneur (son vaisseau), ou du donneur lui-même hors de tout vaisseau.
        Entity GetFrame(const Scene& scene, Entity giver) {
            const Entity station = scene.GetShip(giver);
            return station != NullEntity ? station : giver;
        }

        bool HasHealth(const Scene& scene, Entity entity) {
            if (scene.TryGet<HealthComponent>(entity)) {
                return true;
            }
            bool found = false;
            scene.ForEachChild(entity, [&](Entity child) { found = found || HasHealth(scene, child); });
            return found;
        }
    }

    Entity FindGiver(const Scene& scene, Entity player) {
        if (player == NullEntity || !scene.IsValid(player)) {
            return NullEntity;
        }
        const Math::WorldPosition position = scene.GetWorldPosition(player);
        Entity best = NullEntity;
        double bestDistance = 0.0;
        for (const auto [entity, giver] : scene.GetRegistry().view<const MissionGiverComponent>().each()) {
            const double distance = Math::Length(Math::Delta(position, scene.GetWorldPosition(entity)));
            if (entity != player && distance <= giver.reach && (best == NullEntity || distance < bestDistance)) {
                best = entity;
                bestDistance = distance;
            }
        }
        return best;
    }

    Math::WorldPosition GetTargetArea(const Scene& scene, Entity giver) {
        const Entity frame = GetFrame(scene, giver);
        Math::WorldPosition area = scene.GetWorldPosition(frame);
        const Math::Vector3f forward = scene.GetWorldRotation(frame).Rotate(Math::Vector3f{0.0f, 0.0f, -1.0f});
        area += Math::Vector3d(forward) * static_cast<double>(std::max(scene.Get<MissionGiverComponent>(giver).distance, 0.0f));
        return area;
    }

    std::expected<std::vector<Entity>, std::string> SpawnEnemies(Scene& scene, const ComponentRegistry& components,
                                                                 const SceneSerializer::PrefabReader& prefabs, Entity giver) {
        const MissionGiverComponent mission = scene.Get<MissionGiverComponent>(giver); // copie : la scène va grandir
        const Math::WorldPosition center = GetTargetArea(scene, giver);
        const Math::Quaternionf frame = scene.GetWorldRotation(GetFrame(scene, giver));
        const Math::Vector3f side = frame.Rotate(Math::Vector3f{1.0f, 0.0f, 0.0f});
        const Math::Vector3f forward = frame.Rotate(Math::Vector3f{0.0f, 0.0f, -1.0f});
        // Tournés vers la station : leur avant (-z) vers elle.
        const Math::Quaternionf facing =
            Math::Normalize(frame * Math::Quaternionf::FromAxisAngle({0.0f, 1.0f, 0.0f}, std::numbers::pi_v<float>));
        const int count = std::clamp(mission.enemyCount, 1, 50);
        std::vector<Entity> enemies;
        enemies.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            Entity enemy = NullEntity;
            if (mission.enemyPrefab.empty()) {
                enemy = CreateDrone(scene, i + 1);
            } else {
                auto placed = SceneSerializer::Instantiate(scene, components, mission.enemyPrefab, prefabs);
                if (!placed) {
                    for (const Entity spawned : enemies) {
                        scene.DestroyEntity(spawned);
                    }
                    return std::unexpected(std::move(placed.error()));
                }
                enemy = *placed;
            }
            // En cercle autour du centre, dans le plan de la station ; seul, au centre.
            const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(count);
            const float radius = count > 1 ? std::max(mission.spread, 0.0f) : 0.0f;
            Math::WorldPosition position = center;
            position += Math::Vector3d(side * (std::cos(angle) * radius) + forward * (std::sin(angle) * radius));
            TransformComponent& transform = scene.Get<TransformComponent>(enemy);
            transform.position = position.offset;
            transform.rotation = facing;
            if (position.sector != Math::Vector3i64{}) {
                scene.Add<SectorComponent>(enemy, position.sector);
            } else if (scene.TryGet<SectorComponent>(enemy)) {
                scene.Remove<SectorComponent>(enemy);
            }
            // Un vaisseau ennemi ne tombe pas sans pilote (près d'une planète ou dans un monde à gravité) ; sans
            // points de vie, il serait indestructible et la mission impossible. Sans IA de combat, il en reçoit une :
            // il se défend.
            if (auto* ship = scene.TryGet<ShipComponent>(enemy)) {
                ship->fallsWithoutPilot = false;
                if (!scene.TryGet<CombatAIComponent>(enemy)) {
                    scene.Add<CombatAIComponent>(enemy);
                }
            }
            if (!HasHealth(scene, enemy)) {
                scene.Add<HealthComponent>(enemy, HealthComponent{.health = DefaultHealth});
            }
            enemies.push_back(enemy);
        }
        return enemies;
    }

    int CountAlive(const Scene& scene, std::span<const Entity> enemies) {
        return static_cast<int>(std::ranges::count_if(enemies, [&](Entity enemy) { return scene.IsValid(enemy); }));
    }
}
