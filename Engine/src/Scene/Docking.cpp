#include "Flux/Scene/Docking.hpp"

#include "Flux/Scene/Scene.hpp"
#include "Steering.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Flux::Docking {
    namespace {
        // Le trajet est une suite de points dans le repère du quai, rejoints un à un : à PassTolerance mètres d'un point
        // de passage, on vise le suivant ; les points où l'on s'arrête (au-dessus du quai, posé) sont plus serrés.
        constexpr double PassTolerance = 3.0;
        constexpr double StopTolerance = 0.3;
        constexpr double DockTolerance = 0.05;
        constexpr double ParkedDistance = 1.5; // m : un vaisseau garé à moins de ça d'un quai libre y est amarré par L
        constexpr float AlignTolerance = 2.0f; // °, demi-tour au-dessus du quai terminé
        constexpr float EnterSpeed = 15.0f;    // m/s dans la station
        constexpr float DescendSpeed = 2.0f;   // m/s en se posant sur le quai
        constexpr float RiseSpeed = 5.0f;      // m/s en décollant du quai
        constexpr float ExitSpeed = 60.0f;     // m/s en sortant, vitesse gardée quand le pilote reprend la main
        constexpr double FacingDistance = 20.0; // m : plus loin, on se tourne vers le point avant d'y foncer

        struct Frame
        {
            Math::WorldPosition position;
            Math::Quaternionf rotation;
        };

        Frame GetFrame(const Scene& scene, Entity entity) { return {scene.GetWorldPosition(entity), scene.GetWorldRotation(entity)}; }

        Math::WorldPosition ToWorld(const Frame& frame, const Math::Vector3d& local) {
            Math::WorldPosition position = frame.position;
            position += Math::Vector3d(frame.rotation.Rotate(Math::Vector3f(local)));
            return position;
        }

        Math::Vector3d ToLocal(const Frame& frame, const Math::WorldPosition& position) {
            return Math::Vector3d(frame.rotation.Conjugate().Rotate(Math::Vector3f(Math::Delta(frame.position, position))));
        }

        // Vole vers target : droit dessus, pas plus vite que maxSpeed ni que ce qui permet de s'y arrêter (les
        // rétrofusées freinent à deux fois ShipComponent::acceleration, on n'en compte qu'une : de la marge). Retourne
        // la distance restante.
        double Fly(ShipComponent& ship, const Math::Quaternionf& rotation, const Math::WorldPosition& position,
                   const Math::WorldPosition& target, float maxSpeed, float delta) {
            const Math::Vector3d offset = Math::Delta(position, target);
            const double distance = Math::Length(offset);
            const float acceleration = std::max(ship.acceleration, 0.1f);
            const float speed = std::min(maxSpeed, 0.9f * std::sqrt(2.0f * acceleration * static_cast<float>(distance)));
            const Math::Vector3f wanted = distance > 1.0e-6 ? Math::Vector3f(offset / distance) * speed : Math::Vector3f{};
            const Math::Vector3f current = rotation.Rotate(ship.velocity);
            Math::Vector3f change = wanted - current;
            const float step = 2.0f * acceleration * delta;
            if (const float length = Math::Length(change); length > step) {
                change = change * (step / length);
            }
            ship.velocity = rotation.Conjugate().Rotate(current + change);
            return distance;
        }

        bool IsFree(const Scene& scene, const DockComponent& dock, Entity ship) {
            return dock.ship == NullEntity || dock.ship == ship || !scene.IsValid(dock.ship);
        }

        // Quai réservé par le vaisseau, s'il existe encore et appartient à une station.
        DockComponent* FindDock(Scene& scene, const ShipComponent& ship, Entity& station) {
            station = NullEntity;
            if (ship.dock == NullEntity || !scene.IsValid(ship.dock)) {
                return nullptr;
            }
            auto* dock = scene.TryGet<DockComponent>(ship.dock);
            station = dock ? scene.GetShip(ship.dock) : NullEntity;
            return station != NullEntity ? dock : nullptr;
        }

        // Rend le quai et coupe le pilote automatique ; le vaisseau garde sa vitesse.
        void Release(Scene& scene, ShipComponent& ship) {
            Entity station = NullEntity;
            if (DockComponent* dock = FindDock(scene, ship, station)) {
                dock->ship = NullEntity;
            }
            ship.dock = NullEntity;
            ship.autopilot = AutopilotStage::Off;
        }

        // Posé sur le quai : enfant de la station, exactement à la place du quai dans son repère.
        void Attach(Scene& scene, Entity entity, ShipComponent& ship, Entity station) {
            const Frame stationFrame = GetFrame(scene, station);
            const Frame dockFrame = GetFrame(scene, ship.dock);
            scene.SetParent(entity, station);
            TransformComponent& transform = scene.Get<TransformComponent>(entity);
            transform.position = ToLocal(stationFrame, dockFrame.position);
            transform.rotation = Math::Normalize(stationFrame.rotation.Conjugate() * dockFrame.rotation);
            ship.velocity = {};
            ship.angularVelocity = {};
            ship.cruiseSpeed = 0.0f;
            ship.autopilot = AutopilotStage::Off;
        }

        // Quitte la station : racine de la scène, à la même place dans le monde.
        void Detach(Scene& scene, Entity entity) {
            const Math::WorldPosition position = scene.GetWorldPosition(entity);
            const Math::Quaternionf rotation = scene.GetWorldRotation(entity);
            scene.SetParent(entity, NullEntity);
            TransformComponent& transform = scene.Get<TransformComponent>(entity);
            transform.position = position.offset;
            transform.rotation = rotation;
            if (position.sector != Math::Vector3i64{}) {
                scene.Add<SectorComponent>(entity, position.sector);
            }
        }

        void Hold(ShipComponent& ship) {
            ship.velocity = {};
            ship.angularVelocity = {};
            ship.cruiseSpeed = 0.0f;
            ship.travel = false;
            ship.travelLevel = 0.0f;
            ship.throttle = 0.0f;
            ship.grounded = true;
        }

        void Drive(Scene& scene, Entity entity, ShipComponent& ship, float delta) {
            Entity station = NullEntity;
            DockComponent* dock = FindDock(scene, ship, station);
            if (!dock || station == entity) {
                Release(scene, ship); // quai ou station disparus : le pilote reprend la main
                return;
            }
            ship.travel = false;
            ship.travelLevel = 0.0f;
            ship.cruiseSpeed = 0.0f;
            if (delta <= 0.0f) {
                return; // pause
            }
            const Frame frame = GetFrame(scene, ship.dock);
            const Math::WorldPosition position = scene.GetWorldPosition(entity);
            const Math::Quaternionf rotation = scene.GetWorldRotation(entity);
            const Math::Vector3f up = frame.rotation.Rotate(Math::Vector3f{0.0f, 1.0f, 0.0f});
            const Math::Vector3d local = ToLocal(frame, position);
            const double approach = -static_cast<double>(std::max(dock->approach, 0.0f));
            const double hover = std::max(dock->hover, 0.0f);
            const double clearance = std::max(dock->clearance, 0.0f);

            // Vers un point du repère du quai, le nez tourné vers lui à l'horizontale (comme un vaisseau à décollage
            // vertical, il reste à plat) ; loin, il se tourne d'abord, puis accélère.
            const auto travel = [&](const Math::Vector3d& point, float maxSpeed) {
                const Math::WorldPosition target = ToWorld(frame, point);
                const Math::Vector3f offset(Math::Delta(position, target));
                const Math::Vector3f flat = offset - up * Math::Dot(offset, up);
                const Math::Vector3f forward = rotation.Rotate(Math::Vector3f{0.0f, 0.0f, -1.0f});
                const Math::Vector3f heading = Math::Length(flat) > 1.0f ? Math::Normalize(flat) : Math::Normalize(forward - up * Math::Dot(forward, up));
                Steering::Steer(ship, rotation, Steering::Look(heading, up), delta);
                float facing = 1.0f;
                if (Math::Length(flat) > FacingDistance) {
                    facing = std::clamp(Math::Dot(forward, heading), 0.0f, 1.0f);
                    facing *= facing;
                }
                return Fly(ship, rotation, position, target, maxSpeed * facing, delta);
            };
            // Au-dessus du quai ou posé : orienté comme lui.
            const auto hold = [&](const Math::Vector3d& point, float maxSpeed, float& angle) {
                angle = Steering::Steer(ship, rotation, frame.rotation, delta);
                return Fly(ship, rotation, position, ToWorld(frame, point), maxSpeed, delta);
            };
            const Math::Vector3d approachPoint{0.0, hover, approach};
            const Math::Vector3d hoverPoint{0.0, hover, 0.0};
            float angle = 0.0f;
            ship.throttle = 0.3f; // flammes des réacteurs, discrètes
            switch (ship.autopilot) {
            case AutopilotStage::Climb:
                if (travel({local.x, clearance, local.z}, ship.maxSpeed) < PassTolerance) {
                    ship.autopilot = AutopilotStage::Over;
                }
                break;
            case AutopilotStage::Over:
                if (travel({0.0, clearance, approach}, ship.maxSpeed) < PassTolerance) {
                    ship.autopilot = AutopilotStage::Approach;
                }
                break;
            case AutopilotStage::Under:
                // À sa hauteur, sous la station, jusque devant elle ; il remonte ensuite vers le point d'approche.
                if (travel({local.x, std::min(local.y, -hover), approach}, ship.maxSpeed) < PassTolerance) {
                    ship.autopilot = AutopilotStage::Approach;
                }
                break;
            case AutopilotStage::Approach:
                if (travel(approachPoint, ship.maxSpeed) < PassTolerance) {
                    ship.autopilot = AutopilotStage::Enter;
                }
                break;
            case AutopilotStage::Enter:
                if (travel(hoverPoint, EnterSpeed) < StopTolerance) {
                    ship.autopilot = AutopilotStage::Turn;
                }
                break;
            case AutopilotStage::Turn:
                if (hold(hoverPoint, EnterSpeed, angle) < StopTolerance && angle < AlignTolerance) {
                    ship.autopilot = AutopilotStage::Descend;
                }
                break;
            case AutopilotStage::Descend:
                ship.throttle = 0.0f;
                // Posé sur un sol du monde (station posée sur une planète) un peu au-dessus du quai : amarré aussi.
                if (hold({}, DescendSpeed, angle) < DockTolerance || ship.grounded) {
                    Attach(scene, entity, ship, station);
                    Hold(ship);
                }
                break;
            case AutopilotStage::Rise:
                if (hold(hoverPoint, RiseSpeed, angle) < StopTolerance) {
                    ship.autopilot = AutopilotStage::Exit;
                }
                break;
            case AutopilotStage::Exit:
                if (hold(approachPoint, ExitSpeed, angle) < PassTolerance) {
                    // Hors de la station : le pilote reprend la main, lancé vers l'avant.
                    Release(scene, ship);
                    ship.cruiseSpeed = std::max(-ship.velocity.z, 0.0f);
                }
                break;
            case AutopilotStage::Off:
                break;
            }
        }

        std::string DockName(const Scene& scene, Entity dock) {
            const auto* name = scene.IsValid(dock) ? scene.TryGet<NameComponent>(dock) : nullptr;
            return name ? name->name : std::string("quai");
        }
    }

    bool IsDocked(const Scene& scene, Entity ship) {
        const auto* component = scene.IsValid(ship) ? scene.TryGet<ShipComponent>(ship) : nullptr;
        if (!component || component->autopilot != AutopilotStage::Off || component->dock == NullEntity
            || !scene.IsValid(component->dock) || !scene.TryGet<DockComponent>(component->dock)) {
            return false;
        }
        const Entity station = scene.GetShip(component->dock);
        return station != NullEntity && scene.GetParent(ship) == station;
    }

    bool Dock(Scene& scene, Entity entity, Entity dockEntity) {
        auto* ship = scene.IsValid(entity) ? scene.TryGet<ShipComponent>(entity) : nullptr;
        auto* dock = scene.IsValid(dockEntity) ? scene.TryGet<DockComponent>(dockEntity) : nullptr;
        const Entity station = dock ? scene.GetShip(dockEntity) : NullEntity;
        if (!ship || !dock || station == NullEntity || station == entity || !IsFree(scene, *dock, entity)) {
            return false;
        }
        Release(scene, *ship);
        dock->ship = entity;
        ship->dock = dockEntity;
        Attach(scene, entity, *ship, station);
        Hold(*ship);
        return true;
    }

    ToggleResult Toggle(Scene& scene, Entity entity) {
        auto* ship = scene.IsValid(entity) ? scene.TryGet<ShipComponent>(entity) : nullptr;
        if (!ship) {
            return ToggleResult::NoDock;
        }
        if (IsDocked(scene, entity)) {
            Detach(scene, entity);
            ship->autopilot = AutopilotStage::Rise; // garde son quai jusqu'à être sorti
            ship->grounded = false;
            return ToggleResult::Leaving;
        }
        if (ship->autopilot != AutopilotStage::Off) {
            Release(scene, *ship);
            return ToggleResult::Cancelled;
        }
        // Le quai libre le plus proche, à portée, d'une autre station que le vaisseau lui-même.
        const Math::WorldPosition position = scene.GetWorldPosition(entity);
        Entity best = NullEntity;
        double bestDistance = Range;
        for (const auto [dockEntity, dock] : scene.GetRegistry().view<DockComponent>().each()) {
            const Entity station = scene.GetShip(dockEntity);
            if (station == NullEntity || station == entity || !IsFree(scene, dock, entity)) {
                continue;
            }
            const double distance = Math::Length(Math::Delta(position, scene.GetWorldPosition(dockEntity)));
            if (distance < bestDistance) {
                best = dockEntity;
                bestDistance = distance;
            }
        }
        if (best == NullEntity) {
            return ToggleResult::NoDock;
        }
        // Déjà posé dessus (garé là dans la scène, au début de la partie) : il en repart.
        if (bestDistance < ParkedDistance && Dock(scene, entity, best)) {
            return Toggle(scene, entity);
        }
        DockComponent& dock = scene.Get<DockComponent>(best);
        // Garé ailleurs dans la station (sur le sol d'un hangar) : il sort tout droit par ce quai, sans monter
        // par-dessus la station ni s'y poser.
        if (scene.GetShip(entity) == scene.GetShip(best)) {
            Detach(scene, entity);
            dock.ship = entity;
            ship->dock = best;
            ship->autopilot = AutopilotStage::Exit;
            ship->grounded = false;
            return ToggleResult::Leaving;
        }
        dock.ship = entity;
        ship->dock = best;
        // Devant le point d'approche : droit dessus. Plus bas que la station (sous son sol), par-dessous. Ailleurs (à
        // côté, derrière), par-dessus.
        const Math::Vector3d local = ToLocal(GetFrame(scene, best), position);
        if (local.z <= -static_cast<double>(dock.approach)) {
            ship->autopilot = AutopilotStage::Approach;
        } else if (local.y < -static_cast<double>(dock.hover)) {
            ship->autopilot = AutopilotStage::Under;
        } else {
            ship->autopilot = local.y < dock.clearance ? AutopilotStage::Climb : AutopilotStage::Over;
        }
        return ToggleResult::Docking;
    }

    void Update(Scene& scene, double deltaSeconds) {
        const float delta = static_cast<float>(std::max(deltaSeconds, 0.0));
        for (const auto [entity, ship] : scene.GetRegistry().view<ShipComponent>().each()) {
            if (ship.autopilot != AutopilotStage::Off) {
                Drive(scene, entity, ship, delta);
            } else if (IsDocked(scene, entity)) {
                Hold(ship); // amarré : les commandes du pilote ne le bougent pas
            } else if (ship.dock != NullEntity) {
                Release(scene, ship); // séparé de sa station autrement (éditeur, code du jeu)
            }
        }
    }

    std::string Describe(const Scene& scene, Entity entity) {
        const auto* ship = scene.IsValid(entity) ? scene.TryGet<ShipComponent>(entity) : nullptr;
        if (!ship) {
            return {};
        }
        const std::string dock = DockName(scene, ship->dock);
        switch (ship->autopilot) {
        case AutopilotStage::Climb:
        case AutopilotStage::Over:
            return "Pilote auto : passage au-dessus de la station";
        case AutopilotStage::Under:
            return "Pilote auto : passage sous la station";
        case AutopilotStage::Approach:
            return "Pilote auto : vers " + dock;
        case AutopilotStage::Enter:
        case AutopilotStage::Turn:
            return "Pilote auto : au-dessus du " + dock;
        case AutopilotStage::Descend:
            return "Pilote auto : atterrissage";
        case AutopilotStage::Rise:
        case AutopilotStage::Exit:
            return "Pilote auto : départ";
        case AutopilotStage::Off:
            break;
        }
        return IsDocked(scene, entity) ? "Amarré : " + dock : std::string();
    }
}
