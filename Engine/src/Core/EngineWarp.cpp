// Warp du vaisseau piloté : destinations à l'écran, J vers celle que vise le nez, sortie à l'arrivée.
#include "Flux/Core/Engine.hpp"

#include "Flux/Renderer/Renderer.hpp"
#include "Flux/Scene/Picking.hpp"
#include "Flux/Scene/Ships.hpp"
#include "HudText.hpp"

#include <format>

namespace Flux {
    std::string Engine::GetDestinationName(const Warp::Destination& destination) const {
        if (destination.entity == NullEntity) {
            return m_mission ? "Mission : " + m_mission->title : std::string("Mission");
        }
        const auto* name = m_scene.IsValid(destination.entity) ? m_scene.TryGet<NameComponent>(destination.entity) : nullptr;
        return name ? name->name : std::string("Destination");
    }

    void Engine::UpdateWarp(Entity ship, bool toggle) {
        m_warpAim = -1;
        if (ship == NullEntity) {
            m_warpDestinations.clear();
            return;
        }
        Warp::FindDestinations(m_scene, ship, m_warpDestinations);
        if (m_mission) {
            m_warpDestinations.push_back({NullEntity, m_mission->area, Warp::StationExit});
        }
        ShipComponent& component = m_scene.Get<ShipComponent>(ship);
        // Arrivé, ou arrêté par une planète sur le chemin.
        if (component.warp == WarpStage::Off && !m_warpName.empty()) {
            ShowFlightMessage("Sortie du warp : " + m_warpName);
            m_warpName.clear();
        }
        if (component.warp == WarpStage::Off) {
            m_warpAim = Warp::Aim(m_scene, ship, m_warpDestinations);
        }
        if (!toggle) {
            return;
        }
        if (component.warp != WarpStage::Off) {
            Warp::Stop(m_scene, ship);
            m_warpName.clear();
            ShowFlightMessage("Warp coupé");
            return;
        }
        if (m_warpAim < 0) {
            ShowFlightMessage("Warp : tournez le nez vers une destination");
            return;
        }
        const Warp::Destination& destination = m_warpDestinations[static_cast<std::size_t>(m_warpAim)];
        switch (Warp::Engage(m_scene, ship, destination)) {
        case Warp::EngageResult::Started: m_warpName = GetDestinationName(destination); break;
        case Warp::EngageResult::Unavailable: ShowFlightMessage("Warp impossible ici (posé, amarré, près d'une planète)"); break;
        case Warp::EngageResult::TooClose: ShowFlightMessage("Trop près pour le warp : mode voyage (Tab)"); break;
        }
    }

    void Engine::DrawWarpInfo(Entity player, Math::Vector2f screen) {
        const Entity ship = Ships::GetPilotedShip(m_scene, player);
        if (ship == NullEntity || m_warpDestinations.empty()) {
            return;
        }
        const ShipComponent& component = m_scene.Get<ShipComponent>(ship);
        UI::DrawList& list = m_renderer->GetOverlay();
        constexpr float Size = 16.0f;
        constexpr Math::LinearColor DestinationColor{0.55f, 0.8f, 1.0f, 0.85f};
        constexpr Math::LinearColor AimColor{0.75f, 0.95f, 1.0f, 1.0f};
        const ScreenProjection projection(m_camera, {}, screen);
        // Hors warp : chaque destination, la visée en clair avec la touche. En warp : seulement la sienne.
        for (int i = 0; i < static_cast<int>(m_warpDestinations.size()); ++i) {
            const Warp::Destination& destination = m_warpDestinations[static_cast<std::size_t>(i)];
            const bool aimed = i == m_warpAim;
            if (component.warp != WarpStage::Off && destination.position != component.warpTarget) {
                continue;
            }
            const Math::Vector3d offset = Math::Delta(m_camera.position, destination.position);
            const auto point = projection.ToScreen(offset);
            if (!point) {
                continue;
            }
            const Math::LinearColor color = aimed || component.warp != WarpStage::Off ? AimColor : DestinationColor;
            list.AddCircleOutline(*point, aimed ? 12.0f : 8.0f, color, aimed ? 2.0f : 1.5f);
            std::string text = std::format("{}  {}", GetDestinationName(destination), FormatDistance(Math::Length(offset)));
            if (aimed) {
                text += "   J : warp";
            }
            list.AddText(*point + Math::Vector2f{16.0f, -Size * 0.5f}, text, Size, color);
        }
        if (component.warp == WarpStage::Off) {
            return;
        }
        // Bandeau au centre, sous le réticule : charge, puis distance restante.
        const std::string text = component.warp == WarpStage::Charging
                                     ? std::format("Warp vers {} : charge {:.0f} %", m_warpName,
                                                   std::min(component.warpCharge / Warp::ChargeTime, 1.0f) * 100.0f)
                                     : std::format("Warp vers {} : {}", m_warpName, FormatDistance(Warp::GetRemaining(m_scene, ship)));
        const Math::Vector2f extent = list.MeasureText(text, 18.0f);
        const Math::Vector2f topLeft{(screen.x - extent.x) * 0.5f, screen.y * 0.7f};
        list.AddRoundedRect(topLeft - Math::Vector2f{14.0f, 8.0f}, topLeft + extent + Math::Vector2f{14.0f, 8.0f}, {0.0f, 0.0f, 0.0f, 0.45f}, 8.0f);
        list.AddText(topLeft, text, 18.0f, AimColor);
    }
}
