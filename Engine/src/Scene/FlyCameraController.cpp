#include "Flux/Scene/FlyCameraController.hpp"

#include "Flux/Platform/Window.hpp"
#include "Flux/Scene/Camera.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Flux {
    FlyCameraController::FlyCameraController(const FlyCameraSettings& settings)
        : m_settings(settings), m_speed(settings.speed) {}

    void FlyCameraController::Update(Camera& camera, Window& window, double deltaSeconds) {
        const Input& input = window.GetInput();
        m_active = window.HasFocus() && input.IsMouseButtonDown(MouseButton::Right);
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
            return (input.IsKeyDown(positive) ? 1.0 : 0.0) - (input.IsKeyDown(negative) ? 1.0 : 0.0);
        };
        const double forward = axis(Key::W, Key::S);
        const double right = axis(Key::D, Key::A);
        const double up = axis(Key::E, Key::Q);

        // Le déplacement est calculé en double : un float perdrait les petits pas loin de l'origine.
        Math::Vector3d direction = Math::Vector3d(camera.GetForward()) * forward
                                 + Math::Vector3d(camera.GetRight()) * right
                                 + Math::Vector3d{0.0, 1.0, 0.0} * up;
        direction = Math::Normalize(direction);

        double speed = m_speed;
        if (input.IsKeyDown(Key::LeftShift)) {
            speed *= m_settings.boostFactor;
        }
        camera.position += direction * (speed * deltaSeconds);
    }
}
