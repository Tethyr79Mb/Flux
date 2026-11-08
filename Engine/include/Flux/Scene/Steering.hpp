#pragma once

#include "Flux/Scene/Components.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

// Rotation d'un vaisseau mené par un pilote automatique (amarrage, warp, IA de combat du jeu).
namespace Flux::Steering {
    inline constexpr float Degrees = 180.0f / std::numbers::pi_v<float>;
    inline constexpr float TurnGain = 2.0f; // 1/s : vitesse de rotation par degré restant (arrivée en douceur)

    // Rotation d'un vaisseau qui regarde vers forward (son -z), le haut vers up (unitaires).
    inline Math::Quaternionf Look(const Math::Vector3f& forward, const Math::Vector3f& up) {
        const Math::Quaternionf turn = Math::Quaternionf::FromTo({0.0f, 0.0f, -1.0f}, forward);
        const Math::Vector3f wantedUp = up - forward * Math::Dot(up, forward);
        if (Math::Length(wantedUp) < 1.0e-4f) {
            return turn;
        }
        const Math::Vector3f currentUp = turn.Rotate(Math::Vector3f{0.0f, 1.0f, 0.0f});
        return Math::Normalize(Math::Quaternionf::FromTo(currentUp, Math::Normalize(wantedUp)) * turn);
    }

    // Tourne vers target : vitesse de rotation dans les axes du vaisseau, proportionnelle à l'angle restant, bornée
    // par ShipComponent::turnRate sur chaque axe (en gardant l'axe de rotation) et prise au rythme de turnAcceleration.
    // Retourne l'angle restant (°).
    inline float Steer(ShipComponent& ship, const Math::Quaternionf& rotation, const Math::Quaternionf& target, float delta) {
        Math::Quaternionf error = rotation.Conjugate() * target;
        if (error.w < 0.0f) {
            error = {-error.x, -error.y, -error.z, -error.w}; // le plus court chemin
        }
        const Math::Vector3f axis{error.x, error.y, error.z};
        const float sine = Math::Length(axis);
        const float angle = 2.0f * std::atan2(sine, error.w) * Degrees;
        Math::Vector3f wanted = sine > 1.0e-6f ? axis * (angle * TurnGain / sine) : Math::Vector3f{};
        // Ramenée sous la vitesse de chaque axe d'un seul facteur : le vaisseau tourne toujours vers target.
        float scale = 1.0f;
        const auto bound = [&](float value, float limit) {
            if (std::abs(value) > limit) {
                scale = std::min(scale, limit / std::abs(value));
            }
        };
        bound(wanted.x, ship.turnRate.x);
        bound(wanted.y, ship.turnRate.y);
        bound(wanted.z, ship.turnRate.z);
        wanted = wanted * scale;
        const auto approach = [&](float& value, float goal, float acceleration) {
            const float step = std::max(acceleration, 1.0f) * delta;
            value += std::clamp(goal - value, -step, step);
        };
        approach(ship.angularVelocity.x, wanted.x, ship.turnAcceleration.x);
        approach(ship.angularVelocity.y, wanted.y, ship.turnAcceleration.y);
        approach(ship.angularVelocity.z, wanted.z, ship.turnAcceleration.z);
        return angle;
    }
}
