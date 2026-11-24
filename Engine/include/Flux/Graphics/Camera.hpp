#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Matrix4.hpp"
#include "Flux/Math/Quaternion.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Math/WorldPosition.hpp"

#include <cmath>
#include <numbers>

namespace Flux {
    // Champ de vision réglé par le joueur, en degrés : horizontal pour un écran 16:9, d'où un angle vertical fixe. Un
    // écran plus large voit donc plus sur les côtés (Hor+) au lieu de perdre le haut et le bas.
    constexpr float DefaultFieldOfView = 86.0f;
    constexpr float MinFieldOfView = 60.0f;
    constexpr float MaxFieldOfView = 120.0f;

    // Angle vertical (radians) d'un champ de vision horizontal en 16:9 (degrés).
    inline float VerticalFovFromFieldOfView(float degrees) {
        const float halfWidth = std::tan(degrees * std::numbers::pi_v<float> / 360.0f);
        return 2.0f * std::atan(halfWidth * 9.0f / 16.0f);
    }

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
