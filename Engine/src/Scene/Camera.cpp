#include "Flux/Scene/Camera.hpp"

#include <cmath>

namespace Flux {
    Math::Vector3f Camera::GetForward() const {
        const float cosPitch = std::cos(pitch);
        return {-std::sin(yaw) * cosPitch, std::sin(pitch), -std::cos(yaw) * cosPitch};
    }

    Math::Vector3f Camera::GetRight() const {
        // Horizontal : la caméra ne roule pas.
        return {std::cos(yaw), 0.0f, -std::sin(yaw)};
    }

    Math::Vector3f Camera::GetUp() const {
        return Math::Cross(GetRight(), GetForward());
    }

    Math::Matrix4f Camera::GetViewMatrix() const {
        return Math::Matrix4f::View(GetRight(), GetUp(), GetForward());
    }

    Math::Matrix4f Camera::GetProjectionMatrix(float aspect) const {
        return Math::Matrix4f::PerspectiveReverseZ(verticalFov, aspect, nearPlane);
    }
}
