// IA de combat : un drone de mission patrouille, attaque le vaisseau du joueur qui approche, le touche, rompt quand
// il passe trop près et fuit presque détruit.
#include "Check.hpp"

#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Scene/CombatAI.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Missions.hpp"
#include "Flux/Scene/Scene.hpp"

#include <cmath>

using namespace Flux;

namespace {
    constexpr double Frame = 1.0 / 60.0;

    double Distance(const Math::WorldPosition& a, const Math::WorldPosition& b) { return Math::Length(Math::Delta(a, b)); }
}

bool RunCombatAITests() {
    ComponentRegistry components;
    RegisterEngineComponents(components);
    Scene scene;

    // Un drone de mission (donneur sans prefab) à 5 km devant l'origine.
    const Entity giver = scene.CreateEntity("Donneur");
    scene.Add<MissionGiverComponent>(giver, MissionGiverComponent{.enemyCount = 1, .distance = 5000.0f});
    const SceneSerializer::PrefabReader none = [](std::string_view) -> std::expected<std::string, std::string> {
        return std::unexpected("absent");
    };
    const auto spawned = Missions::SpawnEnemies(scene, components, none, giver);
    CHECK(spawned.has_value() && spawned->size() == 1);
    const Entity drone = spawned->front();
    const Math::WorldPosition home = scene.GetWorldPosition(drone);

    // Le vaisseau du joueur, immobile, à 20 km : hors de vue.
    const Entity player = scene.CreateEntity("Joueur");
    scene.Get<TransformComponent>(player).position = {0.0, 0.0, 15000.0};
    scene.Add<ShipComponent>(player, ShipComponent{.fallsWithoutPilot = false});
    scene.Add<HealthComponent>(player, HealthComponent{.health = 1000.0f});
    const Entity hull = scene.CreateEntity("Coque", player);
    scene.Get<TransformComponent>(hull).scale = {10.0f, 4.0f, 14.0f};
    scene.Add<ColliderComponent>(hull);

    PhysicsWorld world;
    world.Update(scene, 0.0, {});
    std::vector<Projectile> projectiles;
    std::vector<ProjectileHit> hits;
    const auto run = [&](double seconds, auto done) {
        for (int frame = 0; frame < static_cast<int>(std::lround(seconds / Frame)); ++frame) {
            CombatAI::Steer(scene, Frame, player);
            world.Update(scene, Frame, {});
            hits.clear();
            Weapons::UpdateProjectiles(
                projectiles, Frame,
                [&](const Math::WorldPosition& from, const Math::Vector3f& segment, Entity ship) -> std::optional<std::pair<Entity, float>> {
                    if (const auto hit = world.CastRay(scene, from, segment, ship)) {
                        return std::pair{hit->entity, hit->fraction};
                    }
                    return std::nullopt;
                },
                &hits);
            Weapons::ApplyHits(scene, hits, NullEntity);
            CombatAI::Fire(scene, Frame, player, projectiles);
            if (done()) {
                return true;
            }
        }
        return false;
    };
    const CombatAIComponent& ai = scene.Get<CombatAIComponent>(drone);

    // Patrouille : il tourne autour de son point de départ.
    run(10.0, [] { return false; });
    CHECK(ai.state == CombatState::Patrol);
    CHECK(Distance(scene.GetWorldPosition(drone), home) < ai.patrolRadius + 50.0);
    CHECK(projectiles.empty());

    // Le joueur à 1,5 km : il l'attaque, se tourne vers lui, tire et le touche.
    scene.Get<TransformComponent>(player).position = home.offset + Math::Vector3d{0.0, 0.0, 1500.0};
    CHECK(run(Frame, [&] { return ai.state == CombatState::Attack; }));
    CHECK(run(15.0, [&] { return scene.Get<HealthComponent>(player).health < 1000.0f; }));
    CHECK(ai.state == CombatState::Attack || ai.state == CombatState::Break);

    // Collé à lui : il rompt, puis revient.
    CHECK(run(30.0, [&] { return ai.state == CombatState::Break; }));
    CHECK(run(3.5, [&] { return ai.state == CombatState::Attack; }));

    // Presque détruit : il s'enfuit, de plus en plus loin.
    scene.Get<HealthComponent>(drone).health = 10.0f;
    CHECK(run(Frame, [&] { return ai.state == CombatState::Flee; }));
    const double before = Distance(scene.GetWorldPosition(drone), scene.GetWorldPosition(player));
    run(10.0, [] { return false; });
    CHECK(Distance(scene.GetWorldPosition(drone), scene.GetWorldPosition(player)) > before + 500.0);
    return true;
}
