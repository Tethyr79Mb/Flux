#include "Flux/Scene/FlyCameraController.hpp"

#include "Flux/Core/Format.hpp"
#include "Flux/Platform/Window.hpp"
#include "Flux/Scene/Camera.hpp"
#include "Flux/UI/Context.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace Flux {
    FlyCameraController::FlyCameraController(const FlyCameraSettings& settings)
        : m_settings(settings), m_speed(settings.speed) {}

    void FlyCameraController::SetSpeed(double speed) {
        m_speed = std::clamp(speed, m_settings.minSpeed, m_settings.maxSpeed);
    }

    void FlyCameraController::Update(Camera& camera, Window& window, double deltaSeconds, bool mouseAvailable,
                                     bool keyboardAvailable) {
        const Input& input = window.GetInput();
        // Le pilotage commence au clic droit hors de l'interface, puis dure tant que le bouton reste enfoncé,
        // même si le curseur (immobile) se trouve alors sous un panneau.
        const bool held = window.HasFocus() && input.IsMouseButtonDown(MouseButton::Right);
        const bool started = input.WasMouseButtonPressed(MouseButton::Right) && mouseAvailable;
        m_active = held && (m_active || started);
        window.SetCursorLocked(m_active);
        if (!m_active) {
            return;
        }

        const Math::Vector2f mouse = input.GetMouseDelta();
        constexpr float MaxPitch = std::numbers::pi_v<float> * 0.5f - 0.01f; // juste avant la verticale
        camera.yaw = std::remainder(camera.yaw - mouse.x * m_settings.lookSensitivity, 2.0f * std::numbers::pi_v<float>);
        camera.pitch = std::clamp(camera.pitch - mouse.y * m_settings.lookSensitivity, -MaxPitch, MaxPitch);

        if (const float wheel = input.GetWheelDelta(); wheel != 0.0f) {
            m_speed = std::clamp(m_speed * std::pow(m_settings.wheelFactor, static_cast<double>(wheel)),
                                 m_settings.minSpeed, m_settings.maxSpeed);
        }

        const auto axis = [&](Key positive, Key negative) {
            if (!keyboardAvailable) {
                return 0.0;
            }
            return (input.IsKeyDown(positive) ? 1.0 : 0.0) - (input.IsKeyDown(negative) ? 1.0 : 0.0);
        };
        const double forward = axis(Key::W, Key::S);
        const double right = axis(Key::D, Key::A);
        const double up = axis(Key::E, Key::Q);

        // Le déplacement est calculé en double puis reporté dans les secteurs : un float perdrait les petits pas.
        Math::Vector3d direction = Math::Vector3d(camera.GetForward()) * forward
                                 + Math::Vector3d(camera.GetRight()) * right
                                 + Math::Vector3d{0.0, 1.0, 0.0} * up;
        direction = Math::Normalize(direction);

        double speed = m_speed;
        if (keyboardAvailable && input.IsKeyDown(Key::LeftShift)) {
            speed *= m_settings.boostFactor;
        }
        camera.position += direction * (speed * deltaSeconds);
    }

    void FlyCameraController::DrawFields(UI::Context& ui, Camera& camera) {
        ui.LabelText("Secteur", std::format("{}  {}  {}", camera.position.sector.x, camera.position.sector.y,
                                            camera.position.sector.z));
        Math::Vector3d offset = camera.position.offset;
        if (ui.DragDouble3("Position (m)", offset, 0.1, UI::Context::NoMin, UI::Context::NoMax, 2)) {
            camera.position.offset = offset;
            camera.position.Normalize();
        }
        // Pas proportionnel à la valeur : 1 % par pixel, utilisable du cm/s à l'année-lumière par seconde.
        double speed = m_speed;
        if (ui.DragDouble("Vitesse (m/s)", speed, speed * 0.01, 0.0, UI::Context::NoMax, 2)) {
            SetSpeed(speed);
        }
        ui.LabelText("Soit", FormatDistance(m_speed) + "/s");

        ui.Separator();
        ui.TextDisabled("Clic droit maintenu : regarder autour\n"
                        "ZQSD : avancer, E / A : monter / descendre\n"
                        "Maj : accélérer, molette : régler la vitesse");
    }
}
