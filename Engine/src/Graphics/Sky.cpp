#include "Flux/Graphics/Sky.hpp"

#include "Flux/Scene/Components.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Flux {
    namespace {
        // Mélange d'entiers 32 bits : les réglages tirés de la graine (le bruit lui-même est calculé par le GPU,
        // shaders/Sky.slang).
        std::uint32_t Hash(std::uint32_t x) {
            x ^= x >> 16;
            x *= 0x7feb352dU;
            x ^= x >> 15;
            x *= 0x846ca68bU;
            x ^= x >> 16;
            return x;
        }

        float ToUnit(std::uint32_t hash) {
            return static_cast<float>(hash >> 8) * (1.0f / 16777216.0f);
        }

        // Vecteur unitaire tiré de la graine.
        Math::Vector3f RandomDirection(std::uint32_t seed) {
            const float z = ToUnit(Hash(seed)) * 2.0f - 1.0f;
            const float angle = ToUnit(Hash(seed ^ 0x68e31da4U)) * 2.0f * std::numbers::pi_v<float>;
            const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
            return {r * std::cos(angle), r * std::sin(angle), z};
        }
    }

    SkyParameters MakeSkyParameters(const SkyComponent& sky) {
        SkyParameters parameters;
        parameters.seed = Hash(static_cast<std::uint32_t>(sky.seed) ^ 0x5bd1e995U);
        parameters.noiseOffset = Math::Vector3f{ToUnit(Hash(parameters.seed + 1)), ToUnit(Hash(parameters.seed + 2)),
                                                ToUnit(Hash(parameters.seed + 3))} * 1000.0f;
        parameters.galaxyAxis = RandomDirection(parameters.seed + 4);
        // Une direction quelconque du plan de la galaxie : la projection d'une autre direction tirée au hasard.
        Math::Vector3f other = RandomDirection(parameters.seed + 5);
        other -= parameters.galaxyAxis * Math::Dot(other, parameters.galaxyAxis);
        parameters.galaxyCenter = Math::Length(other) > 1.0e-3f ? Math::Normalize(other)
                                                                : Math::Normalize(Math::Cross(parameters.galaxyAxis, Math::Vector3f{0.0f, 1.0f, 0.0f}));
        parameters.nebulaColor = sky.nebulaColor;
        parameters.nebulaColor2 = sky.nebulaColor2;
        parameters.nebula = std::clamp(sky.nebula, 0.0f, 1.0f);
        parameters.galaxy = std::clamp(sky.galaxy, 0.0f, 1.0f);
        return parameters;
    }

    Math::Vector3f SunDirection(float elevationDegrees, float azimuthDegrees) {
        constexpr float RadiansPerDegree = std::numbers::pi_v<float> / 180.0f;
        const float elevation = elevationDegrees * RadiansPerDegree;
        const float azimuth = azimuthDegrees * RadiansPerDegree;
        return {std::cos(elevation) * std::sin(azimuth), std::sin(elevation), std::cos(elevation) * std::cos(azimuth)};
    }

    Math::Vector3f CubeFaceDirection(int face, float u, float v) {
        Math::Vector3f d;
        switch (face) {
            case 0: d = {1.0f, -v, -u}; break;
            case 1: d = {-1.0f, -v, u}; break;
            case 2: d = {u, 1.0f, v}; break;
            case 3: d = {u, -1.0f, -v}; break;
            case 4: d = {u, -v, 1.0f}; break;
            default: d = {-u, -v, -1.0f}; break;
        }
        return Math::Normalize(d);
    }

    void PanoramaUv(const Math::Vector3f& direction, float& u, float& v) {
        constexpr float Pi = std::numbers::pi_v<float>;
        u = std::atan2(direction.x, -direction.z) / (2.0f * Pi) + 0.5f; // le centre de l'image vers -z
        v = std::acos(std::clamp(direction.y, -1.0f, 1.0f)) / Pi;
    }
}
