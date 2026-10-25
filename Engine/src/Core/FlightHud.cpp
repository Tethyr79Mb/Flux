#include "Core/FlightHud.hpp"

#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Planetology/Planet.hpp"
#include "Flux/Scene/Docking.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/Seats.hpp"
#include "Flux/Scene/Ships.hpp"
#include "Flux/UI/DrawList.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace Flux {
    namespace {
        constexpr double NoticeSeconds = 2.5;
    }

    FlightHud::FlightState FlightHud::GetFlightState(Scene& scene, PhysicsWorld* physics, Entity ship) {
        FlightState flight;
        // L'air des planètes ou du monde (WorldComponent), comme pour les objets qui tombent.
        if (physics) {
            flight.air = std::min(1.0f, physics->GetOutsideAir(scene, ship));
        }
        const Math::WorldPosition position = scene.GetWorldPosition(ship);
        const PlanetComponent* nearest = nullptr;
        Math::Vector3d offset{};
        Math::Quaternionf rotation;
        for (const auto [entity, planet] : scene.GetRegistry().view<const PlanetComponent>().each()) {
            if (scene.GetShip(entity) != NullEntity) {
                continue; // décor d'un vaisseau
            }
            const Math::Vector3d candidate = Math::Delta(scene.GetWorldPosition(entity), position);
            if (!nearest || Math::Length(candidate) - planet.radius < Math::Length(offset) - nearest->radius) {
                nearest = &planet;
                offset = candidate;
                rotation = scene.GetWorldRotation(entity);
            }
        }
        // Dans l'espace (au-delà de deux rayons, là où s'arrête sa gravité, voir PlanetGround) : pas d'altitude.
        if (!nearest || Math::Length(offset) < 1.0 || Math::Length(offset) > 2.0 * nearest->radius) {
            return flight;
        }
        if (!m_terrain || m_terrain->GetRadius() != nearest->radius || m_terrain->GetRelief() != nearest->relief
            || m_terrainSeed != nearest->seed || m_terrainWidth != nearest->mountainWidth) {
            m_terrain = std::make_shared<const Planetology::Terrain>(nearest->radius, nearest->relief, nearest->seed, nearest->mountainWidth);
            m_terrainSeed = nearest->seed;
            m_terrainWidth = nearest->mountainWidth;
        }
        flight.nearPlanet = true;
        flight.up = Math::Normalize(offset);
        flight.center = Math::Length(offset);
        const Math::Vector3d direction = rotation.Conjugate().Rotate(flight.up);
        double ground = m_terrain->GetHeight(direction, m_terrain->GetOctaves(m_terrain->GetMaxLevel()));
        if (nearest->ocean) {
            ground = std::max(ground, nearest->seaLevel * 0.01 * nearest->relief);
        }
        flight.altitude = Math::Length(offset) - nearest->radius - ground;
        const double height = std::max(nearest->atmosphereHeight, 100.0f);
        const double above = std::max(Math::Length(offset) - nearest->radius, 0.0);
        flight.boundary = std::max(nearest->atmosphere > 0.0f ? above - height : flight.altitude, 0.0);
        return flight;
    }

    void FlightHud::ShowFlightMessage(std::string message) {
        constexpr double MessageSeconds = 3.0;
        m_message = std::move(message);
        m_messageSeconds = MessageSeconds;
    }

    void FlightHud::DrawFlightInfo(Scene& scene, PhysicsWorld* physics, UI::DrawList& list, Entity player, Math::Vector2f screen,
                                   double deltaSeconds) {
        const Entity seat = Seats::GetPilotSeat(scene, player);
        const Entity ship = seat != NullEntity ? scene.GetShip(seat) : NullEntity;
        if (ship == NullEntity) {
            m_ship = NullEntity; // remesurée en reprenant les commandes
            return;
        }
        const ShipComponent& component = scene.Get<ShipComponent>(ship);
        // Vitesse mesurée sur son déplacement, pas celle que visent ses propulseurs : contre le sol ou un mur, il
        // pousse sans avancer. Lissée sur un quart de seconde ; figée en pause.
        const Math::WorldPosition position = scene.GetWorldPosition(ship);
        if (m_ship != ship) {
            m_ship = ship;
            m_velocity = Math::Vector3d(scene.GetWorldRotation(ship).Rotate(component.velocity));
        } else if (deltaSeconds > 0.0) {
            const Math::Vector3d measured = Math::Delta(m_position, position) / deltaSeconds;
            m_velocity = m_velocity + (measured - m_velocity) * std::min(1.0, deltaSeconds / 0.25);
        }
        m_position = position;
        const Math::Vector3d velocity = m_velocity;
        // Nombre à la française, en m ou km selon sa taille.
        const auto distance = [](double meters, std::string_view unit) {
            std::string text = std::abs(meters) >= 10000.0 ? std::format("{:.1f} k{}", meters / 1000.0, unit)
                                                            : std::format("{:.0f} {}", meters, unit);
            std::ranges::replace(text, '.', ',');
            return text;
        };
        std::string text = "Vitesse  " + distance(Math::Length(velocity), "m/s");
        if (component.travelLevel > 0.0f) {
            text += component.travelLevel < 1.0f ? std::format("\nMode voyage  {:.0f} %", component.travelLevel * 100.0f) : "\nMode voyage";
        }

        if (const FlightState flight = GetFlightState(scene, physics, ship); flight.nearPlanet) {
            text += "\nVerticale  " + distance(Math::Dot(velocity, flight.up), "m/s");
            text += "\nAltitude  " + distance(flight.altitude, "m");
            if (flight.air > 0.001f) {
                text += std::format("\nAir  {:.0f} %", flight.air * 100.0f);
            }
        }
        if (const std::string docking = Docking::Describe(scene, ship); !docking.empty()) {
            text += "\n" + docking;
        }
        if (m_messageSeconds > 0.0) {
            m_messageSeconds -= deltaSeconds;
            text += "\n" + m_message;
        }
        if (!m_extra.empty()) {
            text += "\n" + m_extra;
        }
        const float size = 20.0f;
        const Math::Vector2f extent = list.MeasureText(text, size);
        const Math::Vector2f topLeft{(screen.x - extent.x) * 0.5f, screen.y - extent.y - 40.0f};
        list.AddRoundedRect(topLeft - Math::Vector2f{14.0f, 10.0f}, topLeft + extent + Math::Vector2f{14.0f, 10.0f}, {0.0f, 0.0f, 0.0f, 0.45f}, 8.0f);
        list.AddText(topLeft, text, size, {0.85f, 0.95f, 1.0f, 1.0f});
    }

    void FlightHud::ShowNotice(std::string text) {
        m_notice = std::move(text);
        m_noticeSeconds = NoticeSeconds;
    }

    void FlightHud::DrawNotice(UI::DrawList& list, Math::Vector2f screen, double deltaSeconds, bool loading) {
        if (!loading) {
            m_noticeSeconds -= deltaSeconds; // l'écran de chargement le cache : il attend la fin pour décompter
        }
        // En haut au centre, au-dessus de tout ; s'efface dans la dernière demi-seconde.
        constexpr float NoticeSize = 22.0f;
        const float alpha = static_cast<float>(std::clamp(m_noticeSeconds / 0.5, 0.0, 1.0));
        const Math::Vector2f extent = list.MeasureText(m_notice, NoticeSize);
        const Math::Vector2f position{(screen.x - extent.x) * 0.5f, screen.y * 0.08f};
        list.AddText({position.x + 1.5f, position.y + 1.5f}, m_notice, NoticeSize, {0.0f, 0.0f, 0.0f, 0.6f * alpha});
        list.AddText(position, m_notice, NoticeSize, {1.0f, 1.0f, 1.0f, alpha});
    }
}
