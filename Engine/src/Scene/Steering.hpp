#pragma once

#include "Flux/Scene/Components.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

// Rotation d'un vaisseau mené par un pilote automatique (Docking, Warp).
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
    // par ShipComponent::turnRate et prise au rythme de turnAcceleration. Retourne l'angle restant (°).
    inline float Steer(ShipComponent& ship, const Math::Quaternionf& rotation, const Math::Quaternionf& target, float delta) {
        Math::Quaternionf error = rotation.Conjugate() * target;
        if (error.w < 0.0f) {
            error = {-error.x, -error.y, -error.z, -error.w}; // le plus court chemin
        }
        const Math::Vector3f axis{error.x, error.y, error.z};
        const float sine = Math::Length(axis);
        const float angle = 2.0f * std::atan2(sine, error.w) * Degrees;
        const Math::Vector3f wanted = sine > 1.0e-6f ? axis * (std::min(ship.turnRate, angle * TurnGain) / sine) : Math::Vector3f{};
        const float step = std::max(ship.turnAcceleration, 1.0f) * delta;
        const auto approach = [&](float& value, float goal) { value += std::clamp(goal - value, -step, step); };
        approach(ship.angularVelocity.x, wanted.x);
        approach(ship.angularVelocity.y, wanted.y);
        approach(ship.angularVelocity.z, wanted.z);
        return angle;
    }
}
