#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Matrix4.hpp"
#include "Flux/Math/Quaternion.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Math/WorldPosition.hpp"

#include <numbers>

namespace Flux {
    // Caméra perspective. Repère monde main droite, y vers le haut, mètres.
    // La position est un secteur + un décalage double : le rendu se fait relativement à elle,
    // pour rester précis n'importe où dans la galaxie.
    struct FLUX_API Camera
    {
        Math::WorldPosition position;
        float yaw = 0.0f;   // radians ; 0 regarde vers -z, positif tourne vers la gauche
        float pitch = 0.0f; // radians ; positif regarde vers le haut
        float roll = 0.0f;  // radians ; positif penche la tête vers la gauche (vue cockpit d'un vaisseau qui roule)
        float verticalFov = std::numbers::pi_v<float> / 3.0f; // 60°
        float nearPlane = 0.1f; // pas de plan lointain (reverse-Z infini)

        // Lacet, puis tangage, puis roulis (Quaternionf::FromYawPitchRoll) ; la caméra regarde vers le -z de ce repère.
        Math::Quaternionf GetRotation() const;
        // Lacet, tangage et roulis d'après une rotation (vue depuis un siège).
        void SetRotation(const Math::Quaternionf& rotation);

        Math::Vector3f GetForward() const;
        Math::Vector3f GetRight() const;
        Math::Vector3f GetUp() const;

        Math::Matrix4f GetViewMatrix() const;
        Math::Matrix4f GetProjectionMatrix(float aspect) const;
    };
}
