#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Matrix4.hpp"
#include "Flux/Math/Vector3.hpp"

#include <numbers>

namespace Flux {
    // Caméra perspective. Repère monde main droite, y vers le haut, mètres.
    // La position est en double : le rendu se fait relativement à elle, pour rester précis loin de l'origine.
    struct FLUX_API Camera
    {
        Math::Vector3d position;
        float yaw = 0.0f;   // radians ; 0 regarde vers -z, positif tourne vers la gauche
        float pitch = 0.0f; // radians ; positif regarde vers le haut
        float verticalFov = std::numbers::pi_v<float> / 3.0f; // 60°
        float nearPlane = 0.1f; // pas de plan lointain (reverse-Z infini)

        Math::Vector3f GetForward() const;
        Math::Vector3f GetRight() const;
        Math::Vector3f GetUp() const;

        Math::Matrix4f GetViewMatrix() const;
        Math::Matrix4f GetProjectionMatrix(float aspect) const;
    };
}
