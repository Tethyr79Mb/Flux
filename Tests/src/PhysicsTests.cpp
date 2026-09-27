// Physique (Jolt) : chute sur un sol, personnage qui marche et bute contre un mur, entités ajoutées et retirées.
#include "Check.hpp"

#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Scene/Scene.hpp"

#include <algorithm>
#include <cmath>

using namespace Flux;

namespace {
    constexpr double Frame = 1.0 / 60.0;

    // Dalle de 20 x 0,2 x 20 m dont le dessus est à y = 0 : un cube unité mis à l'échelle.
    Entity AddFloor(Scene& scene) {
        const Entity floor = scene.CreateEntity("Sol");
        TransformComponent& transform = scene.Get<TransformComponent>(floor);
        transform.position = {0.0, -0.1, 0.0};
        transform.scale = {20.0f, 0.2f, 20.0f};
        scene.Add<ColliderComponent>(floor);
        return floor;
    }

    void Run(PhysicsWorld& world, Scene& scene, double seconds, const CharacterInput& input = {}) {
        for (int frame = 0; frame < static_cast<int>(std::lround(seconds / Frame)); ++frame) {
            world.Update(scene, Frame, input);
        }
    }

    CharacterInput Walk(float x, bool run = false) {
        CharacterInput input;
        input.move = {x, 0.0f, 0.0f};
        input.run = run;
        return input;
    }

    bool RunFallTests() {
        Scene scene;
        AddFloor(scene);
        const Entity box = scene.CreateEntity("Caisse");
        scene.Get<TransformComponent>(box).position = {0.0, 5.0, 0.0};
        scene.Add<ColliderComponent>(box).simulatePhysics = true;

        PhysicsWorld world;
        world.Update(scene, 0.0, {});
        CHECK(world.GetBodyCount() == 2);
        Run(world, scene, 3.0);
        // Posée sur le sol : son centre une demi-hauteur au-dessus de y = 0.
        CHECK(std::abs(scene.Get<TransformComponent>(box).position.y - 0.5) < 0.03);

        // Retirer la collision retire le corps ; détruire l'entité aussi.
        scene.Remove<ColliderComponent>(box);
        world.Update(scene, 0.0, {});
        CHECK(world.GetBodyCount() == 1);
        const Entity ball = scene.CreateEntity("Balle");
        scene.Get<TransformComponent>(ball).position = {3.0, 2.0, 0.0};
        ColliderComponent& ballCollider = scene.Add<ColliderComponent>(ball);
        ballCollider.shape = ColliderShape::Sphere;
        ballCollider.simulatePhysics = true;
        world.Update(scene, 0.0, {});
        CHECK(world.GetBodyCount() == 2);
        scene.DestroyEntity(ball);
        world.Update(scene, 0.0, {});
        CHECK(world.GetBodyCount() == 1);

        // Sans gravité (l'espace), un corps reste où il est.
        const Entity drifting = scene.CreateEntity("Débris");
        scene.Get<TransformComponent>(drifting).position = {-3.0, 4.0, 0.0};
        ColliderComponent& driftingCollider = scene.Add<ColliderComponent>(drifting);
        driftingCollider.simulatePhysics = true;
        driftingCollider.gravityFactor = 0.0f;
        Run(world, scene, 1.0);
        CHECK(std::abs(scene.Get<TransformComponent>(drifting).position.y - 4.0) < 1.0e-3);
        return true;
    }

    // Réglages absurdes (tailles nulles, rotation nulle, rebond parfait) : la simulation reste finie.
    bool RunDegenerateTests() {
        Scene scene;
        AddFloor(scene);
        for (int i = 0; i < 4; ++i) {
            const Entity entity = scene.CreateEntity("Poussière");
            TransformComponent& transform = scene.Get<TransformComponent>(entity);
            transform.position = {0.4 * i, 1.0 + i, 0.0};
            if (i == 3) {
                transform.rotation = Math::Quaternionf{0.0f, 0.0f, 0.0f, 0.0f};
            }
            ColliderComponent& collider = scene.Add<ColliderComponent>(entity);
            collider.shape = i % 2 ? ColliderShape::Sphere : ColliderShape::Box;
            collider.size = {0.0f, 0.5f, 0.0f};
            collider.radius = 0.0f;
            collider.center = {1.0f, 1.0f, 1.0f};
            collider.simulatePhysics = true;
            collider.restitution = 1.0f;
            collider.mass = i == 0 ? 2.0f : 0.0f;
        }
        PhysicsWorld world;
        Run(world, scene, 5.0);
        CHECK(world.GetBodyCount() == 5);
        for (const auto [entity, transform] : scene.GetRegistry().view<const TransformComponent>().each()) {
            CHECK(std::isfinite(transform.position.x) && std::isfinite(transform.position.y) && std::isfinite(transform.position.z));
        }
        return true;
    }

    bool RunCharacterTests() {
        Scene scene;
        AddFloor(scene);
        // Mur de 1 m d'épaisseur centré sur x = 3 : sa face avant est à x = 2,5.
        const Entity wall = scene.CreateEntity("Mur");
        TransformComponent& wallTransform = scene.Get<TransformComponent>(wall);
        wallTransform.position = {3.0, 1.0, 0.0};
        wallTransform.scale = {1.0f, 2.0f, 10.0f};
        scene.Add<ColliderComponent>(wall);

        const Entity hero = scene.CreateEntity("Joueur");
        scene.Get<TransformComponent>(hero).position = {0.0, 0.5, 0.0}; // lâché un peu au-dessus du sol
        scene.Add<CharacterComponent>(hero);

        PhysicsWorld world;
        Run(world, scene, 1.0);
        CHECK(world.GetPlayer() == hero);
        // Retombé sur le sol : les pieds à y = 0.
        CHECK(std::abs(scene.Get<TransformComponent>(hero).position.y) < 0.05);

        // Marche vers +x (2 m/s pendant 0,5 s), puis court jusqu'au mur : il s'arrête contre (rayon 0,3 m).
        Run(world, scene, 0.5, Walk(1.0f));
        const double walked = scene.Get<TransformComponent>(hero).position.x;
        CHECK(walked > 0.8 && walked < 1.1);
        Run(world, scene, 3.0, Walk(1.0f, true));
        const double stopped = scene.Get<TransformComponent>(hero).position.x;
        CHECK(stopped > 2.1 && stopped < 2.25);
        // Il s'est tourné vers sa marche : son +z (l'avant d'un modèle glTF) pointe vers +x.
        const Math::Vector3f facing = scene.Get<TransformComponent>(hero).rotation.Rotate(Math::Vector3f{0.0f, 0.0f, 1.0f});
        CHECK(facing.x > 0.95f);

        // Saut : il monte d'environ 1 m, puis retombe au sol.
        CharacterInput jump;
        jump.jump = true;
        world.Update(scene, Frame, jump);
        double highest = 0.0;
        for (int frame = 0; frame < 120; ++frame) {
            world.Update(scene, Frame, {});
            highest = std::max(highest, scene.Get<TransformComponent>(hero).position.y);
        }
        CHECK(highest > 0.8 && highest < 1.3);
        CHECK(std::abs(scene.Get<TransformComponent>(hero).position.y) < 0.05);
        return true;
    }
}

bool RunPhysicsTests() {
    return RunFallTests() && RunDegenerateTests() && RunCharacterTests();
}
