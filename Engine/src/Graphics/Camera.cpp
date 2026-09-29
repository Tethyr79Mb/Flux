#include "Flux/Graphics/Camera.hpp"

#include <algorithm>
#include <cmath>

namespace Flux {
    Math::Vector3f Camera::GetForward() const {
        const float cosPitch = std::cos(pitch);
        return {-std::sin(yaw) * cosPitch, std::sin(pitch), -std::cos(yaw) * cosPitch};
    }

    Math::Vector3f Camera::GetRight() const {
        const Math::Vector3f level{std::cos(yaw), 0.0f, -std::sin(yaw)}; // horizontale sans roulis
        if (roll == 0.0f) {
            return level;
        }
        const Math::Vector3f up = Math::Cross(level, GetForward());
        return level * std::cos(roll) + up * std::sin(roll);
    }

    Math::Vector3f Camera::GetUp() const {
        return Math::Cross(GetRight(), GetForward());
    }

    Math::Quaternionf Camera::GetRotation() const {
        return Math::Quaternionf::FromYawPitchRoll(yaw, pitch, roll);
    }

    void Camera::SetRotation(const Math::Quaternionf& rotation) {
        const Math::Vector3f forward = rotation.Rotate(Math::Vector3f{0.0f, 0.0f, -1.0f});
        pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
        yaw = std::atan2(-forward.x, -forward.z);
        // Roulis : de combien l'axe droit de la rotation penche vers le haut de la caméra sans roulis.
        roll = 0.0f;
        const Math::Vector3f level = GetRight();
        const Math::Vector3f up = GetUp();
        const Math::Vector3f right = rotation.Rotate(Math::Vector3f{1.0f, 0.0f, 0.0f});
        roll = std::atan2(Math::Dot(right, up), Math::Dot(right, level));
    }

    Math::Matrix4f Camera::GetViewMatrix() const {
        return Math::Matrix4f::View(GetRight(), GetUp(), GetForward());
    }

    Math::Matrix4f Camera::GetProjectionMatrix(float aspect) const {
        return Math::Matrix4f::PerspectiveReverseZ(verticalFov, aspect, nearPlane);
    }
}
