// Collisions entre vaisseaux, et pilote automatique d'amarrage : un vaisseau rejoint un quai libre d'une station (par l'avant, ou par-dessus en
// arrivant de derrière), s'y pose et devient son enfant ; on en sort à pied dans la station ; il repart.
#include "Check.hpp"

#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Docking.hpp"
#include "Flux/Scene/SceneSerializer.hpp"
#include "Flux/Scene/Scene.hpp"

#include <cmath>
#include <numbers>

using namespace Flux;

namespace {
    constexpr double Frame = 1.0 / 60.0;

    // Physique et pilote automatique pendant au plus `seconds`, jusqu'à ce que done() soit vrai.
    template <typename Done>
    bool RunUntil(PhysicsWorld& world, Scene& scene, double seconds, Done done) {
        for (int frame = 0; frame < static_cast<int>(std::lround(seconds / Frame)); ++frame) {
            Docking::Update(scene, Frame);
            world.Update(scene, Frame, {});
            if (done()) {
                return true;
            }
        }
        return false;
    }

    Entity AddFighter(Scene& scene, const char* name, Math::Vector3d position) {
        const Entity ship = scene.CreateEntity(name);
        scene.Get<TransformComponent>(ship).position = position;
        scene.Add<ShipComponent>(ship, ShipComponent{.turnRate = 90.0f});
        const Entity hull = scene.CreateEntity("Coque", ship);
        scene.Get<TransformComponent>(hull).scale = {4.0f, 2.0f, 6.0f}; // dessous à 1 m sous l'origine
        scene.Add<ColliderComponent>(hull);
        return ship;
    }

    double Distance(const Math::WorldPosition& a, const Math::WorldPosition& b) { return Math::Length(Math::Delta(a, b)); }

    bool RunDockingTests() {
        Scene scene;
        // Station tournée d'un quart de tour, loin de l'origine : le trajet se calcule dans le repère des quais.
        // Sol du hangar à y = 0 ; deux quais (origine du vaisseau posé à 2,3 m), sortie vers leur -z.
        const Entity station = scene.CreateEntity("Station");
        TransformComponent& stationTransform = scene.Get<TransformComponent>(station);
        stationTransform.position = {500.0, 0.0, 0.0};
        stationTransform.rotation = Math::Quaternionf::FromAxisAngle({0.0f, 1.0f, 0.0f}, 0.5f * std::numbers::pi_v<float>);
        scene.Add<ShipComponent>(station, ShipComponent{.maxSpeed = 0.0f, .acceleration = 0.0f, .fallsWithoutPilot = false});
        const Entity floor = scene.CreateEntity("Sol", station);
        scene.Get<TransformComponent>(floor).position = {0.0, -0.5, 0.0};
        scene.Get<TransformComponent>(floor).scale = {60.0f, 1.0f, 60.0f};
        scene.Add<ColliderComponent>(floor);
        const auto addDock = [&](const char* name, double x) {
            const Entity dock = scene.CreateEntity(name, station);
            scene.Get<TransformComponent>(dock).position = {x, 2.3, 0.0};
            scene.Add<DockComponent>(dock, DockComponent{.approach = 50.0f, .hover = 6.0f, .clearance = 40.0f});
            return dock;
        };
        const Entity dock1 = addDock("Quai 1", -15.0);
        const Entity dock2 = addDock("Quai 2", 15.0);
        // Devant les quais (leur -z est le -x du monde ; le quai 1 est en z = 15) : 300 m plus loin, un peu plus haut.
        const Entity first = AddFighter(scene, "Chasseur", {200.0, 10.0, 10.0});
        PhysicsWorld world;
        world.Update(scene, 0.0, {});

        // Trop loin : rien.
        scene.Get<TransformComponent>(first).position = {500.0 - Docking::Range - 100.0, 10.0, 0.0};
        CHECK(Docking::Toggle(scene, first) == Docking::ToggleResult::NoDock);
        scene.Get<TransformComponent>(first).position = {200.0, 10.0, 10.0};

        // Vers le quai le plus proche (le 1), puis posé dessus, enfant de la station, à sa place exacte.
        CHECK(Docking::Toggle(scene, first) == Docking::ToggleResult::Docking);
        const ShipComponent& firstShip = scene.Get<ShipComponent>(first);
        CHECK(firstShip.dock == dock1 && firstShip.autopilot == AutopilotStage::Approach);
        CHECK(scene.Get<DockComponent>(dock1).ship == first);
        CHECK(RunUntil(world, scene, 120.0, [&] { return Docking::IsDocked(scene, first); }));
        CHECK(scene.GetParent(first) == station);
        CHECK(Math::Length(scene.Get<TransformComponent>(first).position - Math::Vector3d{-15.0, 2.3, 0.0}) < 0.06);
        const Math::Vector3f nose = scene.GetWorldRotation(first).Rotate(Math::Vector3f{0.0f, 0.0f, -1.0f});
        const Math::Vector3f exit = scene.GetWorldRotation(dock1).Rotate(Math::Vector3f{0.0f, 0.0f, -1.0f});
        CHECK(Math::Dot(nose, exit) > 0.999f);
        CHECK(Docking::Describe(scene, first) == "Amarré : Quai 1");
        // Amarré, il ne bouge plus, même si quelqu'un pousse ses commandes.
        scene.Get<ShipComponent>(first).velocity = {0.0f, 0.0f, -20.0f};
        RunUntil(world, scene, 1.0, [] { return false; });
        CHECK(Math::Length(scene.Get<TransformComponent>(first).position - Math::Vector3d{-15.0, 2.3, 0.0}) < 0.06);

        // On en descend à pied : debout sur le sol du hangar (2,3 m sous l'origine du vaisseau), hors de sa coque, le
        // personnage passe dans la station.
        const Entity pilot = scene.CreateEntity("Pilote", first);
        scene.Get<TransformComponent>(pilot).position = {-4.0, -2.3, 0.0};
        scene.Add<CharacterComponent>(pilot).player = false;
        CHECK(RunUntil(world, scene, 2.0, [&] { return scene.GetParent(pilot) == station; }));

        // Un second vaisseau, derrière la station (son +z de quai, le +x du monde) : il passe par-dessus et prend
        // l'autre quai, le premier étant occupé.
        const Entity second = AddFighter(scene, "Second", {700.0, 5.0, 0.0});
        world.Update(scene, 0.0, {});
        CHECK(Docking::Toggle(scene, second) == Docking::ToggleResult::Docking);
        CHECK(scene.Get<ShipComponent>(second).dock == dock2 && scene.Get<ShipComponent>(second).autopilot == AutopilotStage::Climb);
        double highest = 0.0;
        CHECK(RunUntil(world, scene, 180.0, [&] {
            highest = std::max(highest, scene.GetWorldPosition(second).offset.y);
            return Docking::IsDocked(scene, second);
        }));
        CHECK(highest > 40.0);
        CHECK(Math::Length(scene.Get<TransformComponent>(second).position - Math::Vector3d{15.0, 2.3, 0.0}) < 0.06);

        // Le premier repart : racine de la scène, il remonte et sort par l'avant, puis le pilote reprend la main
        // lancé vers l'avant ; le quai est libre.
        CHECK(Docking::Toggle(scene, first) == Docking::ToggleResult::Leaving);
        CHECK(scene.GetParent(first) == NullEntity);
        CHECK(RunUntil(world, scene, 60.0, [&] { return scene.Get<ShipComponent>(first).autopilot == AutopilotStage::Off; }));
        CHECK(scene.Get<ShipComponent>(first).dock == NullEntity && scene.Get<DockComponent>(dock1).ship == NullEntity);
        CHECK(Distance(scene.GetWorldPosition(first), scene.GetWorldPosition(dock1)) > 45.0);
        CHECK(scene.Get<ShipComponent>(first).cruiseSpeed > 5.0f);
        CHECK(scene.GetWorldPosition(first).offset.x < 455.0); // au point d'approche, devant les quais (-x du monde)

        // Coupé en route : le quai est rendu, le pilote reprend la main sur place.
        CHECK(Docking::Toggle(scene, first) == Docking::ToggleResult::Docking);
        CHECK(scene.Get<ShipComponent>(first).dock == dock1);
        CHECK(Docking::Toggle(scene, first) == Docking::ToggleResult::Cancelled);
        CHECK(scene.Get<ShipComponent>(first).autopilot == AutopilotStage::Off && scene.Get<DockComponent>(dock1).ship == NullEntity);
        return true;
    }

    // Un vaisseau à la racine bute sur les murs d'une station et sur la coque d'un autre vaisseau.
    bool RunShipObstacleTests() {
        Scene scene;
        const Entity station = scene.CreateEntity("Station");
        scene.Add<ShipComponent>(station, ShipComponent{.maxSpeed = 0.0f, .fallsWithoutPilot = false});
        const Entity wall = scene.CreateEntity("Mur", station);
        scene.Get<TransformComponent>(wall).position = {0.0, 0.0, 0.0};
        scene.Get<TransformComponent>(wall).scale = {2.0f, 20.0f, 20.0f}; // de x = -1 à 1
        scene.Add<ColliderComponent>(wall);
        // Lancé vers +x à 20 m/s, sans pilote ni chute : il s'arrête contre le mur (sa coque fait 4 m de large).
        const Entity fighter = AddFighter(scene, "Chasseur", {-30.0, 0.0, 0.0});
        ShipComponent& ship = scene.Get<ShipComponent>(fighter);
        ship.fallsWithoutPilot = false;
        ship.velocity = {20.0f, 0.0f, 0.0f};
        // Un autre chasseur, immobile, sur le chemin d'un troisième lancé vers -x.
        const Entity parked = AddFighter(scene, "Garé", {0.0, 0.0, 40.0});
        scene.Get<ShipComponent>(parked).fallsWithoutPilot = false;
        const Entity rammer = AddFighter(scene, "Lancé", {30.0, 0.0, 40.0});
        scene.Get<ShipComponent>(rammer).fallsWithoutPilot = false;
        scene.Get<ShipComponent>(rammer).velocity = {-20.0f, 0.0f, 0.0f};
        PhysicsWorld world;
        world.Update(scene, 0.0, {});
        RunUntil(world, scene, 3.0, [] { return false; });
        const double x = scene.Get<TransformComponent>(fighter).position.x;
        CHECK(x > -3.2 && x < -2.9);
        const double rammed = scene.Get<TransformComponent>(rammer).position.x;
        CHECK(rammed > 3.9 && rammed < 4.2);
        CHECK(std::abs(scene.Get<TransformComponent>(parked).position.x) < 1.0e-6);
        return true;
    }
}

namespace {
    // Un vaisseau garé sans pilote dans le hangar d'une station tombe avec la gravité de son pont et se pose, même
    // dans une scène à planètes où il n'y a pas de gravité dans l'espace.
    bool RunParkedShipTests() {
        Scene scene;
        const Entity planet = scene.CreateEntity("Planète lointaine");
        scene.Get<TransformComponent>(planet).position = {0.0, 0.0, 5.0e8};
        scene.Add<PlanetComponent>(planet);
        const Entity station = scene.CreateEntity("Station");
        scene.Get<TransformComponent>(station).position = {300.0, 0.0, 0.0};
        scene.Add<ShipComponent>(station, ShipComponent{.maxSpeed = 0.0f, .fallsWithoutPilot = false});
        const Entity floor = scene.CreateEntity("Sol", station);
        scene.Get<TransformComponent>(floor).position = {0.0, -0.5, 0.0};
        scene.Get<TransformComponent>(floor).scale = {60.0f, 1.0f, 60.0f};
        scene.Add<ColliderComponent>(floor);
        const Entity roof = scene.CreateEntity("Toit", station);
        scene.Get<TransformComponent>(roof).position = {0.0, 20.5, 0.0};
        scene.Get<TransformComponent>(roof).scale = {60.0f, 1.0f, 60.0f};
        scene.Add<ColliderComponent>(roof);
        // Garé à 6 m du sol (le dessous de sa coque à 1 m sous son origine), et un autre dehors, dans l'espace.
        const Entity parked = AddFighter(scene, "Garé", {300.0, 6.0, 0.0});
        const Entity outside = AddFighter(scene, "Dehors", {300.0, 6.0, 200.0});
        PhysicsWorld world;
        world.Update(scene, 0.0, {});
        CHECK(RunUntil(world, scene, 5.0, [&] { return scene.Get<ShipComponent>(parked).grounded; }));
        CHECK(std::abs(scene.GetWorldPosition(parked).offset.y - 1.0) < 0.1);
        RunUntil(world, scene, 1.0, [] { return false; });
        CHECK(std::abs(scene.GetWorldPosition(parked).offset.y - 1.0) < 0.1); // reste posé
        CHECK(std::abs(scene.GetWorldPosition(outside).offset.y - 6.0) < 0.01);
        return true;
    }
}

namespace {
    // Passage vers le hangar d'une station et retour : le vaisseau, avec le joueur assis à bord, est écrit, retiré de
    // la scène quittée, recréé (mêmes identifiants) dans l'autre, posé sur le quai d'arrivée ; la scène quittée,
    // gardée en texte, revient telle quelle.
    bool RunPassageTests() {
        ComponentRegistry components;
        RegisterEngineComponents(components);
        Scene scene;
        const Entity outsideStation = scene.CreateEntity("Station");
        scene.Add<ShipComponent>(outsideStation, ShipComponent{.maxSpeed = 0.0f, .fallsWithoutPilot = false});
        const Entity field = scene.CreateEntity("Champ bleu", outsideStation);
        scene.Add<PassageComponent>(field, PassageComponent{.scene = "hangar", .arrival = "Plateforme"});
        const Entity ship = AddFighter(scene, "Chasseur", {0.0, 0.0, 50.0});
        const Entity seat = scene.CreateEntity("Siège", ship);
        scene.Add<SeatComponent>(seat).pilot = true;
        const Entity player = scene.CreateEntity("Joueur", ship);
        scene.Add<CharacterComponent>(player).player = true;
        const Uuid shipId = scene.GetId(ship);
        const Uuid seatId = scene.GetId(seat);
        const Uuid playerId = scene.GetId(player);

        // Le hangar : sa station, son quai.
        Scene hangarScene;
        const Entity hangar = hangarScene.CreateEntity("Station - hangar");
        hangarScene.Add<ShipComponent>(hangar, ShipComponent{.maxSpeed = 0.0f, .fallsWithoutPilot = false});
        const Entity platform = hangarScene.CreateEntity("Plateforme", hangar);
        hangarScene.Get<TransformComponent>(platform).position = {0.0, 2.3, 0.0};
        hangarScene.Add<DockComponent>(platform);
        const std::string hangarText = SceneSerializer::SaveToString(hangarScene, components);
        // Garé sur le quai dans la scène (début de partie), sans y être amarré : L l'en fait partir.
        {
            Scene parkedScene;
            CHECK(SceneSerializer::LoadFromString(parkedScene, components, hangarText).has_value());
            const Entity parked = AddFighter(parkedScene, "Garé", {0.3, 2.0, 0.2});
            CHECK(!Docking::IsDocked(parkedScene, parked));
            CHECK(Docking::Toggle(parkedScene, parked) == Docking::ToggleResult::Leaving);
            CHECK(parkedScene.Get<ShipComponent>(parked).autopilot == AutopilotStage::Rise);
            // Garé plus loin sur le sol du hangar, enfant de la station : il sort tout droit, sans monter.
            const Entity aside = AddFighter(parkedScene, "À côté", {25.0, 2.0, 30.0});
            parkedScene.SetParent(aside, parkedScene.FindByName("Station - hangar"));
            CHECK(Docking::Toggle(parkedScene, aside) == Docking::ToggleResult::NoDock); // quai pris par le premier
            parkedScene.DestroyEntity(parked);
            CHECK(Docking::Toggle(parkedScene, aside) == Docking::ToggleResult::Leaving);
            CHECK(parkedScene.Get<ShipComponent>(aside).autopilot == AutopilotStage::Exit);
            CHECK(parkedScene.GetParent(aside) == NullEntity);
        }

        // Entrée.
        const std::string shipText = SceneSerializer::SaveEntity(scene, components, ship, true);
        scene.DestroyEntity(ship);
        const std::string outside = SceneSerializer::SaveToString(scene, components);
        CHECK(SceneSerializer::LoadFromString(scene, components, hangarText).has_value());
        CHECK(SceneSerializer::RestoreEntity(scene, components, shipText).has_value());
        const Entity inside = scene.FindById(shipId);
        CHECK(inside != NullEntity && scene.GetParent(inside) == NullEntity);
        CHECK(scene.GetParent(scene.FindById(playerId)) == inside && scene.GetParent(scene.FindById(seatId)) == inside);
        const Entity dock = scene.FindByName("Plateforme");
        CHECK(Docking::Dock(scene, inside, dock));
        CHECK(Docking::IsDocked(scene, inside) && scene.GetParent(inside) == scene.FindByName("Station - hangar"));
        CHECK(Math::Length(scene.Get<TransformComponent>(inside).position - Math::Vector3d{0.0, 2.3, 0.0}) < 1e-6);
        CHECK(!Docking::Dock(scene, AddFighter(scene, "Autre", {}), dock)); // quai pris

        // Sortie : le vaisseau quitte le hangar, la scène d'origine revient avec sa station et son champ.
        scene.SetParent(inside, NullEntity);
        const std::string backText = SceneSerializer::SaveEntity(scene, components, inside, true);
        scene.DestroyEntity(inside);
        CHECK(SceneSerializer::LoadFromString(scene, components, outside).has_value());
        CHECK(SceneSerializer::RestoreEntity(scene, components, backText).has_value());
        CHECK(scene.FindById(shipId) != NullEntity && scene.FindById(playerId) != NullEntity);
        const Entity backField = scene.FindByName("Champ bleu");
        CHECK(backField != NullEntity && scene.Get<PassageComponent>(backField).arrival == "Plateforme");
        CHECK(scene.FindByName("Plateforme") == NullEntity);
        return true;
    }
}

bool RunDockingAutopilotTests() {
    return RunShipObstacleTests() && RunDockingTests() && RunParkedShipTests() && RunPassageTests();
}
