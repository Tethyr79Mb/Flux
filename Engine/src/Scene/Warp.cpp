#include "Flux/Scene/Warp.hpp"

#include "Flux/Scene/Scene.hpp"
#include "Steering.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Flux::Warp {
    namespace {
        constexpr float ChargeAngle = 3.0f;         // ° : plus de travers, il attend d'être tourné
        constexpr float StartAcceleration = 1000.0f; // m/s² au départ...
        constexpr float GrowthTime = 1.0f;          // ...puis sa vitesse est multipliée par e chaque seconde
        constexpr double BrakeTime = 1.0;           // s : il ne va pas plus vite que la distance restante en BrakeTime
        constexpr double MinSpeed = 2000.0;         // m/s en fin de course, pour finir d'arriver
        constexpr double PlanetMargin = 2000.0;     // m au-dessus de l'atmosphère d'une planète : il sort du warp
        constexpr double PlanetExit = 10000.0;      // m au-dessus de l'atmosphère d'une planète visée

        // Limite d'une planète : le haut de son atmosphère, ou de ses montagnes sans atmosphère.
        double Boundary(const PlanetComponent& planet) {
            return static_cast<double>(planet.radius) + planet.relief + (planet.atmosphere > 0.0f ? planet.atmosphereHeight : 0.0f);
        }

        // Distance à la limite de la planète la plus proche (planètes du monde, pas le décor d'un vaisseau).
        double DistanceToPlanets(const Scene& scene, const Math::WorldPosition& position) {
            double nearest = std::numeric_limits<double>::infinity();
            for (const auto [entity, planet] : scene.GetRegistry().view<const PlanetComponent>().each()) {
                if (scene.GetShip(entity) == NullEntity) {
                    nearest = std::min(nearest, Math::Length(Math::Delta(scene.GetWorldPosition(entity), position)) - Boundary(planet));
                }
            }
            return nearest;
        }

        double Remaining(const Scene& scene, Entity entity, const ShipComponent& ship) {
            return Math::Length(Math::Delta(scene.GetWorldPosition(entity), ship.warpTarget)) - ship.warpExit;
        }

        void Leave(ShipComponent& ship) {
            ship.warp = WarpStage::Off;
            ship.warpCharge = 0.0f;
            // Retour à la vitesse normale, droit devant : la vitesse du warp ne se garde pas.
            if (const float speed = Math::Length(ship.velocity); speed > ship.maxSpeed && speed > 0.0f) {
                ship.velocity = ship.velocity * (ship.maxSpeed / speed);
            }
            ship.cruiseSpeed = std::clamp(-ship.velocity.z, 0.0f, ship.maxSpeed);
        }
    }

    void FindDestinations(const Scene& scene, Entity ship, std::vector<Destination>& destinations) {
        destinations.clear();
        for (const Entity dock : scene.GetRegistry().view<const DockComponent>()) {
            const Entity station = scene.GetShip(dock);
            if (station == NullEntity || station == ship
                || std::ranges::any_of(destinations, [&](const Destination& known) { return known.entity == station; })) {
                continue;
            }
            destinations.push_back({station, scene.GetWorldPosition(station), StationExit});
        }
        for (const auto [entity, planet] : scene.GetRegistry().view<const PlanetComponent>().each()) {
            if (scene.GetShip(entity) == NullEntity) {
                destinations.push_back({entity, scene.GetWorldPosition(entity), Boundary(planet) + PlanetExit});
            }
        }
    }

    int Aim(const Scene& scene, Entity ship, std::span<const Destination> destinations) {
        const Math::WorldPosition position = scene.GetWorldPosition(ship);
        const Math::Vector3f forward = scene.GetWorldRotation(ship).Rotate(Math::Vector3f{0.0f, 0.0f, -1.0f});
        const float limit = std::cos(AimAngle / Steering::Degrees);
        int best = -1;
        float bestCosine = limit;
        for (int i = 0; i < static_cast<int>(destinations.size()); ++i) {
            const Math::Vector3d offset = Math::Delta(position, destinations[i].position);
            const double distance = Math::Length(offset);
            if (distance - destinations[i].exit < MinDistance) {
                continue;
            }
            const float cosine = Math::Dot(Math::Vector3f(offset / distance), forward);
            if (cosine >= bestCosine) {
                best = i;
                bestCosine = cosine;
            }
        }
        return best;
    }

    EngageResult Engage(Scene& scene, Entity entity, const Destination& destination) {
        auto* ship = scene.TryGet<ShipComponent>(entity);
        if (!ship || ship->warpSpeed <= 0.0f || scene.GetParent(entity) != NullEntity || ship->grounded
            || ship->autopilot != AutopilotStage::Off) {
            return EngageResult::Unavailable;
        }
        const Math::WorldPosition position = scene.GetWorldPosition(entity);
        if (DistanceToPlanets(scene, position) < 2.0 * PlanetMargin) {
            return EngageResult::Unavailable; // dans une atmosphère, ou tout près
        }
        if (Math::Length(Math::Delta(position, destination.position)) - destination.exit < MinDistance) {
            return EngageResult::TooClose;
        }
        ship->warp = WarpStage::Charging;
        ship->warpTarget = destination.position;
        ship->warpExit = destination.exit;
        ship->warpCharge = 0.0f;
        ship->travel = false;
        ship->travelLevel = 0.0f;
        return EngageResult::Started;
    }

    void Stop(Scene& scene, Entity entity) {
        if (auto* ship = scene.TryGet<ShipComponent>(entity); ship && ship->warp != WarpStage::Off) {
            Leave(*ship);
        }
    }

    void Update(Scene& scene, double deltaSeconds) {
        const float delta = static_cast<float>(deltaSeconds);
        for (const auto [entity, ship] : scene.GetRegistry().view<ShipComponent>().each()) {
            if (ship.warp == WarpStage::Off) {
                continue;
            }
            if (scene.GetParent(entity) != NullEntity || ship.warpSpeed <= 0.0f || ship.autopilot != AutopilotStage::Off) {
                Leave(ship);
                continue;
            }
            if (delta <= 0.0f) {
                continue; // pause
            }
            const Math::WorldPosition position = scene.GetWorldPosition(entity);
            const Math::Quaternionf rotation = scene.GetWorldRotation(entity);
            const Math::Vector3d offset = Math::Delta(position, ship.warpTarget);
            const double distance = Math::Length(offset);
            const double remaining = distance - ship.warpExit;
            if (remaining <= 0.0 || distance < 1.0e-3) {
                Leave(ship); // arrivé
                continue;
            }
            const Math::Vector3f direction(offset / distance);
            const float angle = Steering::Steer(ship, rotation,
                                                Steering::Look(direction, rotation.Rotate(Math::Vector3f{0.0f, 1.0f, 0.0f})), delta);
            ship.throttle = 1.0f;
            if (ship.warp == WarpStage::Charging) {
                ship.warpCharge += delta;
                if (ship.warpCharge >= ChargeTime && angle < ChargeAngle) {
                    ship.warp = WarpStage::Cruising;
                }
                continue; // garde sa vitesse en se tournant
            }
            // Près d'une planète, il ralentit comme à l'arrivée ; à sa limite, il sort du warp. Une image ne le porte
            // jamais plus loin que la distance qui l'en sépare : il ne passe pas au travers.
            const double planets = DistanceToPlanets(scene, position);
            if (planets < PlanetMargin) {
                Leave(ship);
                continue;
            }
            double limit = std::min({static_cast<double>(ship.warpSpeed) * 1000.0, remaining / BrakeTime, planets / BrakeTime});
            limit = std::max(limit, MinSpeed);
            limit = std::min(limit, remaining / deltaSeconds); // pile à la sortie, pas au-delà
            const double speed = Math::Length(ship.velocity);
            const double grown = std::max(speed + StartAcceleration * deltaSeconds, speed * std::exp(deltaSeconds / GrowthTime));
            const float wanted = static_cast<float>(std::min(grown, limit));
            // Droit sur la destination (pas le long du nez, qui la rattrape) : il y arrive même mal tourné.
            ship.velocity = rotation.Conjugate().Rotate(direction * wanted);
        }
    }

    double GetRemaining(const Scene& scene, Entity entity) {
        const auto* ship = scene.TryGet<ShipComponent>(entity);
        return ship && ship->warp != WarpStage::Off ? std::max(Remaining(scene, entity, *ship), 0.0) : 0.0;
    }
}
