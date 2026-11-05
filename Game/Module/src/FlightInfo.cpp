#include "FlightInfo.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/Seats.hpp>
#include <Flux/Scene/Ships.hpp>
#include <Flux/UI/DrawList.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace Game {
    namespace {
        using namespace Flux;

        constexpr const char* HudSystem = "Infos de vol : affichage";
        constexpr const char* ResetSystem = "Infos de vol : oubli";
        constexpr double MessageSeconds = 3.0;

        struct Extra
        {
            std::string source;
            std::string text; // garde sa capacité d'une image à l'autre : pas d'allocation
        };

        // Vitesse réellement parcourue par le vaisseau piloté : sa position à l'image d'avant et la vitesse mesurée,
        // lissée.
        Entity g_ship = NullEntity;
        Math::WorldPosition g_position;
        Math::Vector3d g_velocity;
        std::string g_message;
        double g_messageSeconds = 0.0;
        std::vector<Extra> g_extras;
        std::string g_text; // texte affiché, gardé d'une image à l'autre

        // Nombre à la française, en m ou km selon sa taille.
        void AppendDistance(std::string& text, double meters, std::string_view unit) {
            const std::size_t start = text.size();
            if (std::abs(meters) >= 10000.0) {
                std::format_to(std::back_inserter(text), "{:.1f} k{}", meters / 1000.0, unit);
            } else {
                std::format_to(std::back_inserter(text), "{:.0f} {}", meters, unit);
            }
            std::ranges::replace(text.begin() + static_cast<std::ptrdiff_t>(start), text.end(), '.', ',');
        }

        void Draw(Engine& engine, double deltaSeconds) {
            Scene& scene = engine.GetScene();
            const Entity seat = Seats::GetPilotSeat(scene, engine.GetPlayer());
            const Entity ship = seat != NullEntity ? scene.GetShip(seat) : NullEntity;
            if (ship == NullEntity) {
                g_ship = NullEntity; // remesurée en reprenant les commandes
                return;
            }
            const ShipComponent& component = scene.Get<ShipComponent>(ship);
            // Vitesse mesurée sur son déplacement, pas celle que visent ses propulseurs : contre le sol ou un mur, il
            // pousse sans avancer. Lissée sur un quart de seconde ; figée en pause.
            const Math::WorldPosition position = scene.GetWorldPosition(ship);
            if (g_ship != ship) {
                g_ship = ship;
                g_velocity = Math::Vector3d(Ships::GetWorldVelocity(scene, ship));
            } else if (deltaSeconds > 0.0) {
                const Math::Vector3d measured = Math::Delta(g_position, position) / deltaSeconds;
                g_velocity = g_velocity + (measured - g_velocity) * std::min(1.0, deltaSeconds / 0.25);
            }
            g_position = position;

            g_text = "Vitesse  ";
            AppendDistance(g_text, Math::Length(g_velocity), "m/s");
            if (component.travelLevel > 0.0f) {
                g_text += component.travelLevel < 1.0f ? std::format("\nMode voyage  {:.0f} %", component.travelLevel * 100.0f) : "\nMode voyage";
            }
            if (const FlightState flight = engine.GetFlightState(ship); flight.nearPlanet) {
                g_text += "\nVerticale  ";
                AppendDistance(g_text, Math::Dot(g_velocity, flight.up), "m/s");
                g_text += "\nAltitude  ";
                AppendDistance(g_text, flight.altitude, "m");
                if (flight.air > 0.001f) {
                    std::format_to(std::back_inserter(g_text), "\nAir  {:.0f} %", flight.air * 100.0f);
                }
            }
            if (g_messageSeconds > 0.0) {
                g_messageSeconds -= deltaSeconds;
                g_text += "\n";
                g_text += g_message;
            }
            for (const Extra& extra : g_extras) {
                if (!extra.text.empty()) {
                    g_text += "\n";
                    g_text += extra.text;
                }
            }
            const Math::Vector2i size = engine.GetWindow().GetSize();
            const Math::Vector2f screen{static_cast<float>(size.x), static_cast<float>(size.y)};
            UI::DrawList& list = engine.GetRenderer().GetOverlay();
            const float textSize = 20.0f;
            const Math::Vector2f extent = list.MeasureText(g_text, textSize);
            const Math::Vector2f topLeft{(screen.x - extent.x) * 0.5f, screen.y - extent.y - 40.0f};
            list.AddRoundedRect(topLeft - Math::Vector2f{14.0f, 10.0f}, topLeft + extent + Math::Vector2f{14.0f, 10.0f},
                                {0.0f, 0.0f, 0.0f, 0.45f}, 8.0f);
            list.AddText(topLeft, g_text, textSize, {0.85f, 0.95f, 1.0f, 1.0f});

            // Manche de la souris : un petit cercle au centre (zone morte), le pointeur et un trait qui l'y relie. Plus le
            // pointeur est loin, plus le vaisseau tourne vite ; ramené au centre, il cesse de tourner.
            const Math::Vector2f center = screen * 0.5f;
            const float reach = 0.18f * screen.y; // bord du cercle du manche, à fond
            constexpr Math::LinearColor StickColor{0.85f, 0.95f, 1.0f, 0.55f};
            list.AddCircleOutline(center, 0.06f * reach, StickColor, 1.5f);
            const Math::Vector2f pointer = engine.GetSteerPointer();
            if (std::abs(pointer.x) + std::abs(pointer.y) > 0.0f) {
                const Math::Vector2f tip = center + pointer * reach;
                list.AddLine(center, tip, {StickColor.r, StickColor.g, StickColor.b, 0.3f}, 1.5f);
                list.AddCircle(tip, 3.5f, StickColor);
            }
        }

        void Reset(Engine&, double) {
            g_ship = NullEntity;
            g_messageSeconds = 0.0;
            g_extras.clear();
        }
    }

    void FlightInfo::SetExtra(std::string_view source, std::string_view text) {
        const auto found = std::ranges::find(g_extras, source, &Extra::source);
        if (found != g_extras.end()) {
            found->text.assign(text);
        } else if (!text.empty()) {
            g_extras.push_back({std::string(source), std::string(text)});
        }
    }

    void FlightInfo::ShowMessage(std::string message) {
        g_message = std::move(message);
        g_messageSeconds = MessageSeconds;
    }

    void RegisterFlightInfo(Engine& engine) {
        engine.AddGameSystem(Engine::GameStage::Hud, HudSystem, Draw);
        engine.AddGameSystem(Engine::GameStage::Reset, ResetSystem, Reset);
    }

    void UnregisterFlightInfo(Engine& engine) {
        engine.RemoveGameSystem(HudSystem);
        engine.RemoveGameSystem(ResetSystem);
        Reset(engine, 0.0);
    }
}
