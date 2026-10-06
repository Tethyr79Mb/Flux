// Physique (Jolt) : chute sur un sol, personnage qui marche et bute contre un mur, entités ajoutées et retirées.
#include "Check.hpp"

#include "Flux/Graphics/Planet.hpp"
#include "Flux/Physics/PhysicsContext.hpp"
#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/Seats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

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
        float longestAir = 0.0f;
        for (int frame = 0; frame < 120; ++frame) {
            world.Update(scene, Frame, {});
            highest = std::max(highest, scene.Get<TransformComponent>(hero).position.y);
            longestAir = std::max(longestAir, scene.Get<CharacterComponent>(hero).airTime);
        }
        CHECK(highest > 0.8 && highest < 1.3);
        // Environ 0,9 s en l'air (4,5 m/s vers le haut, gravité 9,81), puis de nouveau au sol.
        CHECK(longestAir > 0.7f && longestAir < 1.1f && scene.Get<CharacterComponent>(hero).airTime == 0.0f);
        CHECK(std::abs(scene.Get<TransformComponent>(hero).position.y) < 0.05);
        return true;
    }

    // Deux personnages : ni l'un ni l'autre ne décolle, qu'ils soient côte à côte, l'un dans l'autre, ou que le joueur
    // marche droit sur l'autre.
    bool RunTwoCharactersTests() {
        for (const double gap : {1.0, 0.2, 0.0}) {
            Scene scene;
            AddFloor(scene);
            const Entity hero = scene.CreateEntity("Joueur");
            scene.Add<CharacterComponent>(hero);
            const Entity other = scene.CreateEntity("Autre");
            scene.Get<TransformComponent>(other).position = {gap, 0.0, 0.0};
            scene.Add<CharacterComponent>(other);

            PhysicsWorld world;
            double highest = 0.0;
            for (int frame = 0; frame < 300; ++frame) {
                world.Update(scene, Frame, frame < 150 ? Walk(1.0f) : CharacterInput{});
                highest = std::max({highest, scene.Get<TransformComponent>(hero).position.y,
                                    scene.Get<TransformComponent>(other).position.y});
            }
            std::printf("  écart %.1f : plus haut %.3f, joueur y=%.3f, autre y=%.3f\n", gap, highest,
                        scene.Get<TransformComponent>(hero).position.y, scene.Get<TransformComponent>(other).position.y);
            CHECK(highest < 0.3);
        }
        return true;
    }

    // Vaisseau qui avance et vire, sans sol dans le monde : à bord, le joueur, un PNJ et une caisse restent posés
    // sur le pont, et le joueur y marche normalement.
    bool RunShipTests() {
        Scene scene;
        const Entity ship = scene.CreateEntity("Vaisseau");
        scene.Get<TransformComponent>(ship).position = {100.0, 50.0, -30.0};
        scene.Add<ShipComponent>(ship, ShipComponent{.velocity = {0.0f, 0.0f, -20.0f}, .angularVelocity = {0.0f, 30.0f, 0.0f}});
        const Entity deck = scene.CreateEntity("Pont", ship);
        TransformComponent& deckTransform = scene.Get<TransformComponent>(deck);
        deckTransform.position = {0.0, -0.1, 0.0};
        deckTransform.scale = {10.0f, 0.2f, 20.0f};
        scene.Add<ColliderComponent>(deck);
        const Entity hero = scene.CreateEntity("Joueur", ship);
        scene.Get<TransformComponent>(hero).position = {0.0, 0.3, 0.0};
        scene.Add<CharacterComponent>(hero);
        const Entity crew = scene.CreateEntity("PNJ", ship);
        scene.Get<TransformComponent>(crew).position = {-2.0, 0.0, 3.0};
        scene.Add<CharacterComponent>(crew).player = false;
        const Entity crate = scene.CreateEntity("Caisse", ship);
        scene.Get<TransformComponent>(crate).position = {2.0, 1.0, 0.0};
        scene.Add<ColliderComponent>(crate).simulatePhysics = true;

        PhysicsWorld world;
        Run(world, scene, 5.0);
        CHECK(world.GetBodyCount() == 2 && world.GetPlayer() == hero);
        // Le vaisseau a bien bougé (20 m/s pendant 5 s, en virant) et tourné (30°/s).
        const Math::WorldPosition shipPosition = scene.GetWorldPosition(ship);
        const double travelled = Math::Length(Math::Delta(Math::WorldPosition{{}, {100.0, 50.0, -30.0}}, shipPosition));
        CHECK(travelled > 50.0 && travelled < 101.0);
        // À bord, rien n'a glissé ni décollé (positions dans le repère du vaisseau).
        const Math::Vector3d heroLocal = scene.Get<TransformComponent>(hero).position;
        CHECK(std::abs(heroLocal.y) < 0.05 && std::abs(heroLocal.x) < 0.05 && std::abs(heroLocal.z) < 0.05);
        const Math::Vector3d crewLocal = scene.Get<TransformComponent>(crew).position;
        CHECK(std::abs(crewLocal.y) < 0.05 && std::abs(crewLocal.x + 2.0) < 0.05 && std::abs(crewLocal.z - 3.0) < 0.05);
        CHECK(std::abs(scene.Get<TransformComponent>(crate).position.y - 0.5) < 0.05);
        // Dans le monde, le joueur a suivi le vaisseau.
        CHECK(Math::Length(Math::Delta(shipPosition, scene.GetWorldPosition(hero))) < 0.1);

        // Il marche (vers +x du monde, 2 m/s pendant 1 s) : 2 m sur le pont, dans le repère du vaisseau.
        Run(world, scene, 1.0, Walk(1.0f));
        const Math::Vector3d walked = scene.Get<TransformComponent>(hero).position;
        CHECK(std::abs(walked.y) < 0.05);
        const double distance = std::sqrt(walked.x * walked.x + walked.z * walked.z);
        CHECK(distance > 1.7 && distance < 2.2);

        // La caisse sort du vaisseau (plus d'enfant) : son corps passe dans le monde de la scène.
        scene.SetParent(crate, NullEntity);
        world.Update(scene, Frame, {});
        CHECK(world.GetBodyCount() == 2);
        // Le vaisseau disparaît : son monde avec lui.
        scene.DestroyEntity(ship);
        world.Update(scene, Frame, {});
        CHECK(world.GetBodyCount() == 1 && world.GetPlayer() == NullEntity);
        return true;
    }

    // Vaisseau posé (tourné d'un quart de tour) avec un pont 20 cm au-dessus du sol : le joueur descend du pont et
    // marche sur le sol du monde sans le traverser, puis remonte à bord. Il change de parent à chaque fois.
    bool RunBoardingTests() {
        Scene scene;
        AddFloor(scene);
        const Entity ship = scene.CreateEntity("Vaisseau");
        TransformComponent& shipTransform = scene.Get<TransformComponent>(ship);
        shipTransform.position = {5.0, 0.0, 0.0};
        shipTransform.rotation = Math::Quaternionf::FromAxisAngle({0.0f, 1.0f, 0.0f}, std::numbers::pi_v<float> / 2.0f);
        scene.Add<ShipComponent>(ship);
        const Entity deck = scene.CreateEntity("Pont", ship);
        TransformComponent& deckTransform = scene.Get<TransformComponent>(deck);
        deckTransform.position = {0.0, 0.1, 0.0};
        deckTransform.scale = {4.0f, 0.2f, 4.0f}; // x de 3 à 7 dans le monde
        scene.Add<ColliderComponent>(deck);
        const Entity hero = scene.CreateEntity("Joueur", ship);
        scene.Get<TransformComponent>(hero).position = {0.0, 0.2, 0.0};
        scene.Add<CharacterComponent>(hero);

        PhysicsWorld world;
        Run(world, scene, 0.5);
        CHECK(scene.GetParent(hero) == ship);
        CHECK(std::abs(scene.Get<TransformComponent>(hero).position.y - 0.2) < 0.05);

        // Il descend vers +x (2 m/s) : sorti du vaisseau, debout sur le sol.
        Run(world, scene, 1.8, Walk(1.0f));
        Run(world, scene, 0.5);
        CHECK(scene.GetParent(hero) == NullEntity);
        const Math::Vector3d outside = scene.GetWorldPosition(hero).offset;
        CHECK(std::abs(outside.y) < 0.05 && outside.x > 7.5);
        // Du dehors, le pont est solide (copie dans le monde) : pas de corps en plus comptés.
        CHECK(world.GetBodyCount() == 2);

        // Il revient (vers -x) : il monte sur le pont et repasse à bord.
        Run(world, scene, 2.0, Walk(-1.0f));
        Run(world, scene, 0.5);
        CHECK(scene.GetParent(hero) == ship);
        const Math::Vector3d aboard = scene.GetWorldPosition(hero).offset;
        CHECK(std::abs(aboard.y - 0.2) < 0.05 && aboard.x > 3.0 && aboard.x < 7.0);
        return true;
    }

    // Caméra qui suit le joueur : les murs et les coques l'arrêtent, pas les caisses, les personnages ni la coque
    // du vaisseau où il se trouve.
    bool RunCameraTests() {
        const auto addBox = [](Scene& scene, const char* name, Entity parent, Math::Vector3d position, Math::Vector3f scale) {
            const Entity entity = scene.CreateEntity(name, parent);
            TransformComponent& transform = scene.Get<TransformComponent>(entity);
            transform.position = position;
            transform.scale = scale;
            scene.Add<ColliderComponent>(entity);
            return entity;
        };
        constexpr float Radius = 0.2f;
        const Math::Vector3f forward{0.0f, 0.0f, 1.0f};

        Scene scene;
        AddFloor(scene);
        const Entity hero = scene.CreateEntity("Joueur");
        scene.Add<CharacterComponent>(hero);
        addBox(scene, "Mur", NullEntity, {0.0, 2.5, 5.0}, {10.0f, 5.0f, 0.2f}); // face avant à z = 4,9
        const Entity crate = addBox(scene, "Caisse", NullEntity, {0.0, 1.6, 2.0}, {1.0f, 1.0f, 1.0f});
        scene.Get<ColliderComponent>(crate).simulatePhysics = true;
        const Entity crew = scene.CreateEntity("PNJ");
        scene.Get<TransformComponent>(crew).position = {0.0, 0.0, 3.0};
        scene.Add<CharacterComponent>(crew).player = false;

        PhysicsWorld world;
        world.Update(scene, 0.0, {}); // corps créés, rien n'a bougé
        const Math::WorldPosition eye{{}, {0.0, 1.6, 0.0}};
        CHECK(std::abs(world.CastCamera(scene, hero, eye, forward, 10.0f, Radius) - 4.7f) < 0.05f);
        CHECK(world.CastCamera(scene, hero, eye, forward, 3.0f, Radius) == 3.0f);
        CHECK(world.CastCamera(scene, hero, eye, forward * -1.0f, 3.0f, Radius) == 3.0f);

        // Un vaisseau (coque pleine de 6 x 4 x 6 m) au loin derrière : sa coque arrête la caméra.
        const Entity ship = addBox(scene, "Vaisseau", NullEntity, {0.0, 2.0, -10.0}, {6.0f, 4.0f, 6.0f});
        scene.Add<ShipComponent>(ship);
        world.Update(scene, 0.0, {});
        CHECK(std::abs(world.CastCamera(scene, hero, eye, forward * -1.0f, 20.0f, Radius) - 6.8f) < 0.05f);

        // Le joueur monte à bord (le vaisseau tourné d'un quart de tour) : la coque ne gêne plus, la cloison
        // intérieure si, dans le repère du vaisseau.
        TransformComponent& shipTransform = scene.Get<TransformComponent>(ship);
        shipTransform.rotation = Math::Quaternionf::FromAxisAngle({0.0f, 1.0f, 0.0f}, std::numbers::pi_v<float> / 2.0f);
        shipTransform.scale = {1.0f, 1.0f, 1.0f};
        scene.Get<ColliderComponent>(ship).size = {6.0f, 4.0f, 6.0f};
        addBox(scene, "Cloison", ship, {0.0, 0.0, 2.0}, {6.0f, 4.0f, 0.2f}); // locale +z : +x dans le monde
        scene.SetParent(hero, ship);
        scene.Get<TransformComponent>(hero).position = {0.0, -2.0, 0.0};
        world.Update(scene, 0.0, {});
        const Math::WorldPosition inside{{}, {0.0, 1.6, -10.0}};
        CHECK(std::abs(world.CastCamera(scene, hero, inside, {1.0f, 0.0f, 0.0f}, 10.0f, Radius) - 1.7f) < 0.05f);
        CHECK(world.CastCamera(scene, hero, inside, {-1.0f, 0.0f, 0.0f}, 2.5f, Radius) == 2.5f);

        // Vue extérieure du vaisseau piloté : ni sa coque, ni son décor de bord (la cloison, ni sa copie dans le monde
        // extérieur) n'arrêtent la caméra ; le mur, si.
        CHECK(world.CastCamera(scene, hero, inside, {1.0f, 0.0f, 0.0f}, 10.0f, Radius, ship) == 10.0f);
        CHECK(std::abs(world.CastCamera(scene, hero, inside, forward, 30.0f, Radius, ship) - 14.7f) < 0.05f);
        return true;
    }

    // Poste de pilotage : le joueur s'y assoit (E), y reste posé pendant que le vaisseau accélère et vire sous ses
    // commandes, puis se relève devant le siège.
    bool RunSeatTests() {
        Scene scene;
        const Entity ship = scene.CreateEntity("Vaisseau");
        scene.Get<TransformComponent>(ship).position = {0.0, 10.0, 0.0};
        scene.Add<ShipComponent>(ship);
        const Entity deck = scene.CreateEntity("Pont", ship);
        TransformComponent& deckTransform = scene.Get<TransformComponent>(deck);
        deckTransform.position = {0.0, -0.1, 0.0};
        deckTransform.scale = {10.0f, 0.2f, 20.0f};
        scene.Add<ColliderComponent>(deck);
        const Entity seat = scene.CreateEntity("Poste de pilotage", ship);
        scene.Get<TransformComponent>(seat).position = {0.0, 0.0, -3.0};
        scene.Add<SeatComponent>(seat).pilot = true;
        const Entity hero = scene.CreateEntity("Joueur", ship);
        scene.Get<TransformComponent>(hero).position = {3.0, 0.0, -2.0};
        scene.Add<CharacterComponent>(hero);

        PhysicsWorld world;
        const Seats::DurationsFunction durations = [](Entity) { return Seats::Durations{1.0f, 1.0f}; };
        const auto frames = [&](double seconds, const SeatInput& input) {
            for (int frame = 0; frame < static_cast<int>(std::lround(seconds / Frame)); ++frame) {
                Seats::Update(scene, world.GetPlayer(), Frame, input, durations);
                world.Update(scene, Frame, {});
            }
        };
        const SeatInput interact{.interact = true};
        frames(0.5, {});
        // Trop loin (plus de 1,5 m) : rien.
        frames(Frame, interact);
        const CharacterComponent& character = scene.Get<CharacterComponent>(hero);
        CHECK(character.seat == NullEntity && character.seatState == SeatState::Standing);

        // Il s'approche (vers -x, 1,4 s à 2 m/s), puis s'assoit, posé sur le siège dès le début de l'animation.
        Run(world, scene, 1.4, Walk(-1.0f));
        frames(0.3, {});
        frames(Frame, interact);
        CHECK(character.seat == seat && character.seatState == SeatState::SittingDown);
        frames(0.5, {});
        const Math::Vector3d onSeat = scene.Get<TransformComponent>(hero).position;
        CHECK(std::abs(onSeat.x) < 0.01 && std::abs(onSeat.z + 3.0) < 0.01);
        CHECK(Seats::GetPilotSeat(scene, hero) == NullEntity); // pas encore assis
        frames(0.6, {});
        CHECK(character.seatState == SeatState::Seated && Seats::GetPilotSeat(scene, hero) == seat);

        // Aux commandes : 2 s de poussée à 10 m/s² (vitesse de croisière 20 m/s, suivie à 10 m/s² près), puis le
        // vaisseau roule à droite tant que E est tenue, sa vitesse de rotation atteinte à 360 °/s².
        frames(2.0, {.thrust = 1.0f});
        const ShipComponent& shipComponent = scene.Get<ShipComponent>(ship);
        CHECK(std::abs(shipComponent.velocity.z + 20.0f) < 0.5f && std::abs(shipComponent.cruiseSpeed - 20.0f) < 0.5f);
        frames(Frame, {.roll = 1.0f});
        CHECK(std::abs(shipComponent.angularVelocity.z + 6.0f) < 1.0e-3f); // 360 °/s² pendant 1/60 s
        frames(0.25, {.roll = 1.0f});
        CHECK(shipComponent.angularVelocity.z == -shipComponent.turnRate && std::abs(shipComponent.velocity.z + 20.0f) < 0.5f);
        frames(0.25, {});
        CHECK(shipComponent.angularVelocity == Math::Vector3f{});
        // Souris : le vaisseau vise la rotation de ce qu'elle a bougé pendant l'image (nez vers le haut), sans
        // dépasser sa vitesse de rotation ; immobile, il cesse de tourner.
        frames(Frame, {.mousePitch = 0.1f * static_cast<float>(Frame)});
        CHECK(std::abs(shipComponent.angularVelocity.x - 0.1f * 180.0f / std::numbers::pi_v<float>) < 1.0e-3f
              && shipComponent.angularVelocity.y == 0.0f); // en °/s
        // Virage serré (90 °/s pendant 0,5 s, bien plus vite que ses propulseurs ne corrigent).
        scene.Get<ShipComponent>(ship).turnRate = 90.0f;
        frames(0.5, {.mouseYaw = 100.0f});
        CHECK(shipComponent.angularVelocity.y == shipComponent.turnRate);
        // Inertie : il a tourné à gauche, sa vitesse va encore vers l'ancienne direction (sa droite), puis les
        // propulseurs le remettent dans l'axe à la vitesse de croisière.
        CHECK(shipComponent.velocity.x > 5.0f);
        scene.Get<ShipComponent>(ship).turnRate = 30.0f;
        frames(3.0, {});
        CHECK(shipComponent.angularVelocity == Math::Vector3f{} && std::abs(shipComponent.velocity.x) < 1.0e-3f
              && std::abs(shipComponent.velocity.z + 20.0f) < 0.5f);
        // Glissement et montée : il prend sa vitesse de glissement (10 m/s, 1 s à 10 m/s²) tant que les touches sont
        // tenues, puis revient à l'arrêt sur ces axes ; la vitesse de croisière reste.
        frames(1.0, {.strafe = 1.0f, .lift = 1.0f});
        CHECK(std::abs(shipComponent.velocity.x - shipComponent.strafeSpeed) < 1.0e-3f
              && std::abs(shipComponent.velocity.y - shipComponent.strafeSpeed) < 1.0e-3f);
        frames(1.0, {});
        CHECK(std::abs(shipComponent.velocity.x) < 1.0e-3f && std::abs(shipComponent.velocity.y) < 1.0e-3f
              && std::abs(shipComponent.velocity.z + 20.0f) < 0.5f);
        // Espace : il freine jusqu'à l'arrêt (20 m/s à 10 m/s² : 2 s), et ne repart pas.
        frames(1.0, {.brake = true});
        CHECK(std::abs(shipComponent.velocity.z + 10.0f) < 0.5f);
        frames(1.5, {.brake = true});
        frames(0.5, {});
        CHECK(shipComponent.velocity == Math::Vector3f{} && shipComponent.cruiseSpeed == 0.0f);
        // Il a suivi le vaisseau sans bouger de son siège.
        const Math::Vector3d still = scene.Get<TransformComponent>(hero).position;
        CHECK(std::abs(still.x) < 0.01 && std::abs(still.z + 3.0) < 0.01 && std::abs(still.y) < 0.01);
        CHECK(Math::Length(Math::Delta(scene.GetWorldPosition(seat), scene.GetWorldPosition(hero))) < 0.01);

        // Il se relève : toujours posé pendant l'animation, puis debout un demi-mètre devant le siège.
        frames(Frame, interact);
        CHECK(character.seatState == SeatState::StandingUp);
        frames(1.1, {});
        CHECK(character.seat == NullEntity && character.seatState == SeatState::Standing);
        frames(0.5, {});
        const Math::Vector3d stood = scene.Get<TransformComponent>(hero).position;
        CHECK(std::abs(stood.x) < 0.05 && std::abs(stood.z + 3.5) < 0.05 && std::abs(stood.y) < 0.05);

        // Sortie réglée derrière le siège (cockpit étroit) : il se relève 1,2 m derrière.
        scene.Get<SeatComponent>(seat).exit = {0.0f, 0.0f, 1.2f};
        frames(Frame, interact);
        frames(1.1, {});
        CHECK(character.seatState == SeatState::Seated);
        frames(Frame, interact);
        frames(1.6, {});
        const Math::Vector3d behind = scene.Get<TransformComponent>(hero).position;
        CHECK(character.seatState == SeatState::Standing);
        CHECK(std::abs(behind.x) < 0.05 && std::abs(behind.z + 1.8) < 0.05 && std::abs(behind.y) < 0.05);
        return true;
    }

    // Siège à grande portée (chasseur) : du dehors, E monte à bord, droit sur le siège ; en se relevant, le joueur
    // sort au sol à côté du vaisseau et repasse dans le monde du dehors.
    bool RunSeatBoardingTests() {
        Scene scene;
        AddFloor(scene);
        const Entity ship = scene.CreateEntity("Chasseur");
        scene.Get<TransformComponent>(ship).position = {0.0, 2.3, 0.0};
        scene.Add<ShipComponent>(ship);
        const Entity deck = scene.CreateEntity("Plancher", ship);
        TransformComponent& deckTransform = scene.Get<TransformComponent>(deck);
        deckTransform.position = {0.0, -0.1, 0.0};
        deckTransform.scale = {3.0f, 0.2f, 6.0f};
        scene.Add<ColliderComponent>(deck);
        const Entity seat = scene.CreateEntity("Poste de pilotage", ship);
        scene.Get<TransformComponent>(seat).position = {0.0, 0.0, -1.0};
        SeatComponent& component = scene.Add<SeatComponent>(seat);
        component.pilot = true;
        component.reach = 8.0f;
        component.exit = {-3.0f, -2.3f, 0.0f}; // au sol, à gauche du vaisseau
        const Entity hero = scene.CreateEntity("Joueur");
        scene.Get<TransformComponent>(hero).position = {5.0, 0.0, 0.0};
        scene.Add<CharacterComponent>(hero);

        PhysicsWorld world;
        const auto frames = [&](double seconds, const SeatInput& input) {
            for (int frame = 0; frame < static_cast<int>(std::lround(seconds / Frame)); ++frame) {
                Seats::Update(scene, world.GetPlayer(), Frame, input, {});
                world.Update(scene, Frame, {});
            }
        };
        frames(0.5, {});
        const CharacterComponent& character = scene.Get<CharacterComponent>(hero);
        // A 5,6 m du siège, dehors : E le met aux commandes, à bord du chasseur.
        frames(Frame, {.interact = true});
        CHECK(character.seat == seat && character.seatState == SeatState::Seated);
        CHECK(scene.GetParent(hero) == ship);
        frames(0.2, {});
        CHECK(Math::Length(Math::Delta(scene.GetWorldPosition(seat), scene.GetWorldPosition(hero))) < 0.01);
        CHECK(Seats::GetPilotSeat(scene, hero) == seat);

        // Il se relève : au sol à côté du vaisseau, puis rendu au monde du dehors.
        frames(Frame, {.interact = true});
        CHECK(character.seat == NullEntity && character.seatState == SeatState::Standing);
        frames(1.0, {});
        CHECK(scene.GetParent(hero) == NullEntity);
        const Math::Vector3d outside = Math::Delta({}, scene.GetWorldPosition(hero));
        CHECK(std::abs(outside.x + 3.0) < 0.05 && std::abs(outside.y) < 0.05 && std::abs(outside.z + 1.0) < 0.05);

        // Remonte aux commandes. Stabilisation à plat : décolle, s'incline à droite, lâche le roulis ; les ailes reviennent à plat (sans
        // l'aide, l'inclinaison reste).
        frames(Frame, {.interact = true});
        CHECK(Seats::GetPilotSeat(scene, hero) == seat);
        const Math::Vector3f up{0.0f, 1.0f, 0.0f};
        const auto bank = [&] {
            const Math::Vector3f local = scene.GetWorldRotation(ship).Conjugate().Rotate(up);
            return std::atan2(local.x, local.y) * 180.0f / std::numbers::pi_v<float>;
        };
        frames(1.0, {.lift = 1.0f, .up = up});
        frames(0.5, {.roll = 1.0f, .up = up});
        const float banked = bank();
        CHECK(banked < -8.0f);
        frames(3.0, {.up = up});
        CHECK(std::abs(bank()) < 1.0f);
        ShipComponent& flight = scene.Get<ShipComponent>(ship);
        flight.levelAssist = false;
        frames(0.5, {.roll = 1.0f, .up = up});
        frames(0.2, {.up = up}); // le roulis s'arrête
        const float kept = bank();
        frames(1.0, {.up = up});
        CHECK(kept < -8.0f && std::abs(bank() - kept) < 1.0f);
        flight.levelAssist = true;
        frames(3.0, {.up = up});
        CHECK(std::abs(bank()) < 1.0f);
        return true;
    }

    // Sol en maillage (ColliderShape::Mesh) : un carré de 10 m en deux triangles, dans un .gltf à côté de son .bin.
    void WriteMeshFloor(const std::filesystem::path& directory) {
        const float positions[] = {-5.0f, 0.0f, -5.0f, 5.0f, 0.0f, -5.0f, 5.0f, 0.0f, 5.0f, -5.0f, 0.0f, 5.0f};
        const std::uint32_t indices[] = {0, 2, 1, 0, 3, 2}; // antihoraires vus du dessus
        std::vector<char> buffer(sizeof(positions) + sizeof(indices));
        std::memcpy(buffer.data(), positions, sizeof(positions));
        std::memcpy(buffer.data() + sizeof(positions), indices, sizeof(indices));
        std::ofstream(directory / "sol.bin", std::ios::binary).write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::string json = R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
            "accessors": [{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-5, 0, -5], "max": [5, 0, 5]},
                          {"bufferView": 1, "componentType": 5125, "count": 6, "type": "SCALAR"}],
            "bufferViews": [{"buffer": 0, "byteLength": 48}, {"buffer": 0, "byteOffset": 48, "byteLength": 24}],
            "buffers": [{"byteLength": 72, "uri": "sol.bin"}]})";
        std::ofstream(directory / "sol.gltf", std::ios::binary) << json;
    }

    bool RunMeshTests() {
        const std::filesystem::path directory = std::filesystem::temp_directory_path() / "FluxPhysicsTests";
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        std::filesystem::create_directories(directory);
        WriteMeshFloor(directory);

        Scene scene;
        // Le carré agrandi 2 fois (20 m) : un personnage lâché à 8 m du centre, hors du carré d'origine, s'y pose.
        const Entity floor = scene.CreateEntity("Sol");
        scene.Get<TransformComponent>(floor).scale = {2.0f, 1.0f, 2.0f};
        ColliderComponent& collider = scene.Add<ColliderComponent>(floor);
        collider.shape = ColliderShape::Mesh;
        collider.mesh = "sol.gltf";
        collider.simulatePhysics = true; // impossible pour un maillage : reste immobile
        const Entity hero = scene.CreateEntity("Joueur");
        scene.Get<TransformComponent>(hero).position = {8.0, 2.0, 8.0};
        scene.Add<CharacterComponent>(hero);
        // Le même carré à 1,2 m, qui n'arrête que la caméra (une verrière) : le personnage le traverse en tombant.
        const Entity glass = scene.CreateEntity("Verrière");
        TransformComponent& glassTransform = scene.Get<TransformComponent>(glass);
        glassTransform.position = {0.0, 1.2, 0.0};
        glassTransform.scale = {2.0f, 1.0f, 2.0f};
        ColliderComponent& glassCollider = scene.Add<ColliderComponent>(glass);
        glassCollider.shape = ColliderShape::Mesh;
        glassCollider.mesh = "sol.gltf";
        glassCollider.cameraOnly = true;
        // Sans fichier (ni maillage, ni modèle) : pas de collision, sans planter.
        const Entity empty = scene.CreateEntity("Vide");
        scene.Get<TransformComponent>(empty).position = {0.0, -50.0, 0.0};
        scene.Add<ColliderComponent>(empty).shape = ColliderShape::Mesh;
        // Fichier absent : de même.
        const Entity missing = scene.CreateEntity("Absent");
        scene.Get<TransformComponent>(missing).position = {0.0, -60.0, 0.0};
        ColliderComponent& missingCollider = scene.Add<ColliderComponent>(missing);
        missingCollider.shape = ColliderShape::Mesh;
        missingCollider.mesh = "absent.glb";

        PhysicsWorld world;
        world.SetAssetsDirectory(directory);
        Run(world, scene, 1.5);
        const Math::Vector3d landed = scene.Get<TransformComponent>(hero).position;
        CHECK(std::abs(landed.y) < 0.05 && std::abs(landed.x - 8.0) < 0.05);
        CHECK(scene.Get<TransformComponent>(floor).position == Math::Vector3d{});
        CHECK(world.GetBodyCount() == 2);
        // La caméra s'y arrête, par-dessus comme par-dessous (envers des faces, vu d'un cockpit).
        constexpr float Radius = 0.2f;
        const Math::WorldPosition above{{}, {8.0, 3.0, 8.0}};
        const Math::WorldPosition below{{}, {8.0, 0.5, 8.0}};
        CHECK(std::abs(world.CastCamera(scene, hero, above, {0.0f, -1.0f, 0.0f}, 5.0f, Radius) - 1.6f) < 0.05f);
        CHECK(std::abs(world.CastCamera(scene, hero, below, {0.0f, 1.0f, 0.0f}, 5.0f, Radius) - 0.5f) < 0.05f);

        // Contexte partagé (celui du moteur) : Prepare crée les corps sans avancer la simulation ; un second monde
        // (changement de scène, nouveau lancement) reprend les maillages déjà lus au lieu de les relire.
        const auto context = std::make_shared<PhysicsContext>();
        context->SetAssetsDirectory(directory);
        scene.Get<TransformComponent>(hero).position = {8.0, 2.0, 8.0};
        {
            PhysicsWorld first(context);
            first.Prepare(scene);
            CHECK(first.GetBodyCount() == 2 && first.GetPlayer() == hero);
            CHECK(scene.Get<TransformComponent>(hero).position.y == 2.0); // rien n'a bougé
        }
        CHECK(context->GetMeshCount() == 2); // sol.gltf et absent.glb (illisible, gardé pour ne pas le relire)
        PhysicsWorld second(context);
        second.Prepare(scene);
        CHECK(second.GetBodyCount() == 2 && context->GetMeshCount() == 2);
        Run(second, scene, 1.5);
        CHECK(std::abs(scene.Get<TransformComponent>(hero).position.y) < 0.05);
        std::filesystem::remove_all(directory, error);
        return true;
    }
}

namespace {
    // Vaisseau et décor : il se pose sur le sol au lieu de le traverser, glisse le long, ne pique pas du nez dedans,
    // et repart vers le haut. Sans collision à lui, il passe partout.
    bool RunShipCollisionTests() {
        Scene scene;
        AddFloor(scene); // dessus à y = 0
        const Entity ship = scene.CreateEntity("Vaisseau");
        scene.Get<TransformComponent>(ship).position = {0.0, 5.0, 0.0};
        ShipComponent& component = scene.Add<ShipComponent>(ship);
        const Entity hull = scene.CreateEntity("Coque", ship);
        scene.Get<TransformComponent>(hull).scale = {4.0f, 2.0f, 6.0f}; // dessous à 1 m sous l'origine
        scene.Add<ColliderComponent>(hull);
        const Entity ghost = scene.CreateEntity("Fantôme");
        scene.Get<TransformComponent>(ghost).position = {30.0, 5.0, 0.0};
        scene.Add<ShipComponent>(ghost).velocity = {0.0f, -10.0f, 0.0f};

        PhysicsWorld world;
        world.Update(scene, Frame, {}); // corps créés
        component.velocity = {0.0f, -10.0f, 0.0f};
        Run(world, scene, 1.0);
        const Math::Vector3d landed = scene.Get<TransformComponent>(ship).position;
        CHECK(std::abs(landed.y - 1.0) < 0.05 && std::abs(component.velocity.y) < 1.0e-3f);
        CHECK(scene.Get<TransformComponent>(ghost).position.y < -4.0);

        // En avançant tout en poussant vers le bas : il roule sur le sol.
        component.velocity = {0.0f, -5.0f, -10.0f};
        Run(world, scene, 0.5);
        const Math::Vector3d rolled = scene.Get<TransformComponent>(ship).position;
        CHECK(std::abs(rolled.y - 1.0) < 0.05 && std::abs(rolled.z + 5.0) < 0.1);

        // Nez vers le sol, posé : la rotation est refusée.
        component.velocity = {};
        component.angularVelocity = {-30.0f, 0.0f, 0.0f};
        Run(world, scene, 0.5);
        const Math::Vector3f nose = scene.Get<TransformComponent>(ship).rotation.Rotate(Math::Vector3f{0.0f, 0.0f, -1.0f});
        CHECK(nose.y > -0.02f);

        // Posé, il ne vire pas non plus.
        const Math::Quaternionf parked = scene.Get<TransformComponent>(ship).rotation;
        component.angularVelocity = {0.0f, 30.0f, 0.0f};
        Run(world, scene, 0.5);
        CHECK(component.grounded && scene.Get<TransformComponent>(ship).rotation == parked);

        // Vers le haut : il décolle.
        component.angularVelocity = {};
        component.velocity = {0.0f, 5.0f, 0.0f};
        Run(world, scene, 1.0);
        CHECK(scene.Get<TransformComponent>(ship).position.y > 5.5 && !component.grounded);

        // Stabilisateur : penché de 20° sur le côté, il se remet à plat en approchant du sol et se pose bien à plat.
        scene.Get<TransformComponent>(ship).rotation = Math::Quaternionf::FromAxisAngle({0.0f, 0.0f, 1.0f}, 20.0f * std::numbers::pi_v<float> / 180.0f);
        component.velocity = {0.0f, -2.0f, 0.0f};
        Run(world, scene, 4.0);
        const Math::Vector3f up = scene.Get<TransformComponent>(ship).rotation.Rotate(Math::Vector3f{0.0f, 1.0f, 0.0f});
        CHECK(up.y > 0.9999f && std::abs(scene.Get<TransformComponent>(ship).position.y - 1.0) < 0.05 && component.grounded);
        return true;
    }
}

namespace {
    // Sur une planète : on se tient debout sur son sol, vers son centre, on y marche ; une caisse tombe vers son
    // centre et s'y pose ; un vaisseau s'y pose. Tout ça à 60 km de l'origine (le monde se recentre sur le joueur).
    bool RunPlanetSurfaceTests() {
        Scene scene;
        const Entity planet = scene.CreateEntity("Planète");
        const PlanetComponent& planetComponent = scene.Add<PlanetComponent>(planet);
        const Planet::Terrain terrain(planetComponent.radius, planetComponent.relief, planetComponent.seed);
        const std::uint32_t octaves = terrain.GetOctaves(terrain.GetMaxLevel());
        const auto ground = [&](const Math::Vector3d& direction) {
            return terrain.GetRadius() + terrain.GetHeight(direction, octaves);
        };
        // Un endroit presque plat (moins de 5 cm de dénivelé sur 3 m), pour que rien n'y glisse.
        Math::Vector3d up{1.0, 0.0, 0.0};
        for (int i = 0; i < 200; ++i) {
            const double angle = i * 0.01;
            const Math::Vector3d candidate = Math::Normalize(Math::Vector3d{std::cos(angle), 0.3 * std::sin(3.0 * angle), std::sin(angle)});
            const Math::Vector3d side = Math::Normalize(Math::Cross(candidate, Math::Vector3d{0.0, 1.0, 0.0}));
            const Math::Vector3d along = Math::Cross(side, candidate);
            const double h = ground(candidate);
            bool flat = true;
            for (const Math::Vector3d& offset : {side * 1.5, side * -1.5, along * 1.5, along * -1.5}) {
                flat = flat && std::abs(ground(Math::Normalize(candidate * h + offset)) - h) < 0.05;
            }
            if (flat && h > 0.0) {
                up = candidate;
                break;
            }
        }
        const Math::Vector3d east = Math::Normalize(Math::Cross(up, Math::Vector3d{0.0, 1.0, 0.0}));
        const Math::Vector3d north = Math::Cross(east, up);
        const auto altitude = [&](Entity entity) {
            const Math::Vector3d position = scene.Get<TransformComponent>(entity).position;
            return Math::Length(position) - ground(Math::Normalize(position));
        };

        const Entity hero = scene.CreateEntity("Joueur");
        scene.Get<TransformComponent>(hero).position = up * (ground(up) + 1.0);
        scene.Add<CharacterComponent>(hero);
        const Entity box = scene.CreateEntity("Caisse");
        const Math::Vector3d boxDirection = Math::Normalize(up * ground(up) + north * 3.0);
        scene.Get<TransformComponent>(box).position = boxDirection * (ground(boxDirection) + 3.0);
        scene.Add<ColliderComponent>(box).simulatePhysics = true;

        PhysicsWorld world;
        world.Prepare(scene); // sol fait autour du joueur et de la caisse
        Run(world, scene, 2.0);
        // Debout sur le sol, tourné selon la verticale de l'endroit.
        CHECK(std::abs(altitude(hero)) < 0.1);
        const Math::Vector3f heroUp = scene.Get<TransformComponent>(hero).rotation.Rotate(Math::Vector3f{0.0f, 1.0f, 0.0f});
        CHECK(Math::Dot(heroUp, Math::Vector3f(up)) > 0.999f);
        CHECK(Math::Dot(world.GetUp(scene, hero), Math::Vector3f(up)) > 0.999f);
        // La caisse est tombée vers le centre et s'est posée (son centre à une demi-hauteur du sol).
        CHECK(std::abs(altitude(box) - 0.5) < 0.15);

        // Il marche vers l'est (2 m/s pendant 2 s) en restant sur le sol.
        const Math::Vector3d start = scene.Get<TransformComponent>(hero).position;
        Run(world, scene, 2.0, CharacterInput{Math::Vector3f(east), false, false});
        const Math::Vector3d walked = scene.Get<TransformComponent>(hero).position - start;
        CHECK(Math::Dot(walked, east) > 3.5 && Math::Dot(walked, east) < 4.2);
        CHECK(std::abs(altitude(hero)) < 0.1);

        // Un vaisseau 200 m plus loin, à plat sur la planète, descend et se pose (dessous de sa coque à 1 m sous lui).
        const Math::Vector3d shipDirection = Math::Normalize(up * ground(up) + east * 200.0);
        const Entity ship = scene.CreateEntity("Vaisseau");
        TransformComponent& shipTransform = scene.Get<TransformComponent>(ship);
        shipTransform.position = shipDirection * (ground(shipDirection) + 6.0);
        shipTransform.rotation = Math::Quaternionf::FromTo({0.0f, 1.0f, 0.0f}, Math::Vector3f(shipDirection));
        ShipComponent& shipComponent = scene.Add<ShipComponent>(ship);
        const Entity hull = scene.CreateEntity("Coque", ship);
        scene.Get<TransformComponent>(hull).scale = {4.0f, 2.0f, 6.0f};
        scene.Add<ColliderComponent>(hull);
        world.Update(scene, Frame, {});
        shipComponent.velocity = {0.0f, -5.0f, 0.0f};
        Run(world, scene, 2.0);
        CHECK(shipComponent.grounded);
        const double shipAltitude = altitude(ship);
        CHECK(shipAltitude > 0.9 && shipAltitude < 1.5);
        return true;
    }
}

bool RunPhysicsTests() {
    return RunFallTests() && RunDegenerateTests() && RunCharacterTests() && RunTwoCharactersTests() && RunShipTests()
        && RunBoardingTests() && RunCameraTests() && RunSeatTests() && RunSeatBoardingTests() && RunMeshTests()
        && RunShipCollisionTests() && RunPlanetSurfaceTests();
}
