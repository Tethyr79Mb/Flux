// Warp : vers la destination visée, sans collision ni arrêt avant elle, en contournant les planètes sur le chemin.
#include "../Check.hpp"

#include "Docking.hpp"
#include "Warp.hpp"

#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"

#include <cmath>
#include <numbers>
#include <vector>

using namespace Flux;
using namespace Game;

namespace {
    constexpr double Frame = 1.0 / 60.0;

    double Distance(const Math::WorldPosition& a, const Math::WorldPosition& b) { return Math::Length(Math::Delta(a, b)); }

    Entity AddFighter(Scene& scene) {
        const Entity ship = scene.CreateEntity("Chasseur");
        scene.Add<ShipComponent>(ship, ShipComponent{.turnRate = {90.0f, 90.0f, 90.0f}, .turnAcceleration = {360.0f, 360.0f, 360.0f}, .fallsWithoutPilot = false});
        scene.Add<WarpDriveComponent>(ship);
        const Entity hull = scene.CreateEntity("Coque", ship);
        scene.Get<TransformComponent>(hull).scale = {4.0f, 2.0f, 6.0f};
        scene.Add<ColliderComponent>(hull);
        return ship;
    }

    Entity AddStation(Scene& scene, const Math::WorldPosition& position) {
        const Entity station = scene.CreateEntity("Station");
        scene.Get<TransformComponent>(station).position = position.offset;
        if (position.sector != Math::Vector3i64{}) {
            scene.Add<SectorComponent>(station, position.sector);
        }
        scene.Add<ShipComponent>(station, ShipComponent{.maxSpeed = 0.0f, .fallsWithoutPilot = false});
        scene.Add<DockComponent>(scene.CreateEntity("Quai", station));
        return station;
    }

    // Sans physique : le vaisseau avance et tourne de sa vitesse, comme le ferait PhysicsWorld sans obstacle.
    void Integrate(Scene& scene, Entity entity, double delta) {
        const ShipComponent& ship = scene.Get<ShipComponent>(entity);
        TransformComponent& transform = scene.Get<TransformComponent>(entity);
        if (const float speed = Math::Length(ship.angularVelocity); speed > 0.0f) {
            const float angle = speed * std::numbers::pi_v<float> / 180.0f * static_cast<float>(delta);
            transform.rotation = Math::Normalize(transform.rotation * Math::Quaternionf::FromAxisAngle(ship.angularVelocity, angle));
        }
        const SectorComponent* sector = scene.TryGet<SectorComponent>(entity);
        Math::WorldPosition world{sector ? sector->sector : Math::Vector3i64{}, transform.position};
        world += Math::Vector3d(transform.rotation.Rotate(ship.velocity)) * delta;
        transform.position = world.offset;
        if (world.sector != Math::Vector3i64{}) {
            scene.Add<SectorComponent>(entity, world.sector);
        }
    }

    // Avec la physique : une station à 500 km, un mur sur le chemin.
    bool RunWarpTripTests() {
        Scene scene;
        const Entity station = AddStation(scene, {{}, {0.0, 0.0, -500000.0}});
        const Entity wall = scene.CreateEntity("Mur");
        scene.Get<TransformComponent>(wall).position = {0.0, 0.0, -300.0};
        scene.Get<TransformComponent>(wall).scale = {200.0f, 200.0f, 2.0f};
        scene.Add<ColliderComponent>(wall);
        const Entity fighter = AddFighter(scene);
        PhysicsWorld world;
        world.Update(scene, 0.0, {});

        std::vector<Warp::Destination> destinations;
        Warp::FindDestinations(scene, fighter, destinations);
        CHECK(destinations.size() == 1 && destinations[0].entity == station);
        CHECK(Warp::Aim(scene, fighter, destinations) == 0);
        // Nez de l'autre côté : rien de visé.
        scene.Get<TransformComponent>(fighter).rotation = Math::Quaternionf::FromAxisAngle({0.0f, 1.0f, 0.0f}, std::numbers::pi_v<float>);
        CHECK(Warp::Aim(scene, fighter, destinations) == -1);
        // Trop près : le mode voyage suffit.
        CHECK(Warp::Engage(scene, fighter, {station, {{}, {0.0, 0.0, -10000.0}}, Warp::StationExit}) == Warp::EngageResult::TooClose);

        // Parti à l'envers : il se retourne en chargeant, file à travers le mur et sort à 4 km de la station, à vitesse
        // normale.
        CHECK(Warp::Engage(scene, fighter, destinations[0]) == Warp::EngageResult::Started);
        CHECK(scene.Get<ShipComponent>(fighter).autopilot && !scene.Get<ShipComponent>(fighter).passThrough); // charge
        double fastest = 0.0;
        double seconds = 0.0;
        bool passedThrough = false;
        while (scene.Get<WarpDriveComponent>(fighter).stage != WarpStage::Off && seconds < 60.0) {
            Warp::Update(scene, Frame);
            passedThrough |= scene.Get<ShipComponent>(fighter).passThrough;
            world.Update(scene, Frame, {});
            fastest = std::max(fastest, static_cast<double>(Math::Length(scene.Get<ShipComponent>(fighter).velocity)));
            seconds += Frame;
        }
        const ShipComponent& ship = scene.Get<ShipComponent>(fighter);
        CHECK(passedThrough && !ship.autopilot && !ship.passThrough); // la physique le reprend à la sortie
        CHECK(seconds < 25.0);
        CHECK(fastest > 50000.0);
        CHECK(std::abs(Distance(scene.GetWorldPosition(fighter), scene.GetWorldPosition(station)) - Warp::StationExit) < 50.0);
        CHECK(Math::Length(ship.velocity) <= ship.maxSpeed + 0.01f);
        return true;
    }

    // Sans physique : sur plusieurs secteurs, autour d'une planète sur le chemin, puis vers elle.
    bool RunWarpPlanetTests() {
        Scene scene;
        const Math::WorldPosition far{{}, {0.0, 0.0, -3.0e9}};
        const Entity station = AddStation(scene, far);
        const Entity planet = scene.CreateEntity("Planète");
        scene.Get<TransformComponent>(planet).position = {0.0, 0.0, -1.0e9};
        PlanetComponent& planetComponent = scene.Add<PlanetComponent>(planet);
        planetComponent.radius = 600000.0f;
        const double boundary = planetComponent.radius + planetComponent.relief + planetComponent.atmosphereHeight;
        const Entity fighter = AddFighter(scene);

        // Au plus près de la planète pendant le trajet, au-dessus de son atmosphère.
        double lowest = 1.0e30;
        const auto fly = [&](double limit) {
            double seconds = 0.0;
            while (scene.Get<WarpDriveComponent>(fighter).stage != WarpStage::Off && seconds < limit) {
                Warp::Update(scene, Frame);
                Integrate(scene, fighter, Frame);
                if (scene.IsValid(planet)) {
                    lowest = std::min(lowest, Distance(scene.GetWorldPosition(fighter), scene.GetWorldPosition(planet)) - boundary);
                }
                seconds += Frame;
            }
            return seconds;
        };
        std::vector<Warp::Destination> destinations;
        Warp::FindDestinations(scene, fighter, destinations);
        CHECK(destinations.size() == 2);

        // La planète en plein sur la route : il la contourne sans s'arrêter et va jusqu'à la station, à 3 millions de km.
        CHECK(Warp::Engage(scene, fighter, {station, far, Warp::StationExit}) == Warp::EngageResult::Started);
        CHECK(fly(120.0) < 90.0);
        CHECK(scene.Get<WarpDriveComponent>(fighter).stage == WarpStage::Off);
        CHECK(std::abs(Distance(scene.GetWorldPosition(fighter), far) - Warp::StationExit) < 50.0);
        CHECK(scene.GetWorldPosition(fighter).sector != Math::Vector3i64{});
        CHECK(lowest > 5000.0);

        // Vers la planète elle-même : sortie 10 km au-dessus de son atmosphère.
        const Math::WorldPosition planetCenter = scene.GetWorldPosition(planet);
        CHECK(Warp::Engage(scene, fighter, {planet, planetCenter, boundary + 10000.0}) == Warp::EngageResult::Started);
        fly(120.0);
        CHECK(scene.Get<WarpDriveComponent>(fighter).stage == WarpStage::Off);
        CHECK(std::abs(Distance(scene.GetWorldPosition(fighter), planetCenter) - boundary - 10000.0) < 50.0);
        return true;
    }
}

// Anciennes scènes : la vitesse de warp était dans le vaisseau (« ship.warpSpeed »). Un vaisseau qui en avait reçoit le
// moteur de warp du jeu ; une station (0) n'en reçoit pas.
static bool RunWarpDriveConversionTests() {
    ComponentRegistry components;
    RegisterEngineComponents(components);
    const std::size_t engineCount = components.GetCount();
    RegisterWarpDrive(components);
    const char* text = R"({"format": "flux.scene", "version": 1, "entities": [
        {"id": "0000000000000001", "name": "Chasseur", "ship": {"maxSpeed": 200, "warpSpeed": 5000}},
        {"id": "0000000000000002", "name": "Station", "ship": {"maxSpeed": 0, "warpSpeed": 0}}]})";
    Scene scene;
    CHECK(SceneSerializer::LoadFromString(scene, components, text).has_value());
    const Entity fighter = scene.FindById(Uuid{1});
    const Entity station = scene.FindById(Uuid{2});
    CHECK(scene.Get<ShipComponent>(fighter).maxSpeed == 200.0f);
    CHECK(scene.TryGet<WarpDriveComponent>(fighter) && scene.Get<WarpDriveComponent>(fighter).speed == 5000.0f);
    CHECK(scene.TryGet<ShipComponent>(station) && !scene.TryGet<WarpDriveComponent>(station));
    // Réécrite : le moteur de warp à part, plus de champ dans le vaisseau.
    const std::string saved = SceneSerializer::SaveToString(scene, components);
    CHECK(saved.find("warpDrive") != std::string::npos && saved.find("warpSpeed") == std::string::npos);
    // Jeu déchargé : la conversion part avec son composant (son code était dans la DLL).
    components.Truncate(engineCount);
    CHECK(components.Find("ship")->movedFields.empty());
    return true;
}

bool RunWarpTests() { return RunWarpTripTests() && RunWarpPlanetTests() && RunWarpDriveConversionTests(); }
