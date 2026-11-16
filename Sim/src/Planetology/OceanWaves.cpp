#include "Flux/Planetology/OceanWaves.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace Flux::Planetology {
    namespace {
        constexpr float Pi = std::numbers::pi_v<float>;
        // Une cascade garde les ondes jusqu'à BandOverlap fois la plus longue de la suivante : assez de points par
        // onde dans la suivante pour qu'elle les dessine sans crénelage.
        constexpr float BandOverlap = 6.0f;
        // Ondes plus courtes que quelques centimètres effacées : elles ne seraient qu'un bruit au pixel.
        constexpr float ShortWaveLength = 0.02f;

        // Hachage entier (PCG) : mêmes vagues sur toutes les machines pour une graine.
        std::uint32_t Hash(std::uint32_t value) {
            const std::uint32_t state = value * 747796405u + 2891336453u;
            const std::uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
            return (word >> 22u) ^ word;
        }

        // Deux tirages indépendants d'une loi normale (Box-Muller) pour l'onde (m, n).
        std::array<float, 2> Gaussian(std::uint32_t seed, std::uint32_t cascade, std::uint32_t m, std::uint32_t n) {
            const std::uint32_t a = Hash(seed ^ Hash(cascade * 0x9E3779B9u ^ Hash(m + OceanWaveSize * n)));
            const std::uint32_t b = Hash(a ^ 0x85EBCA6Bu);
            const float u1 = (static_cast<float>(a >> 8) + 1.0f) / 16777217.0f; // ]0, 1] : pas de log(0)
            const float u2 = static_cast<float>(b >> 8) / 16777216.0f;
            const float radius = std::sqrt(-2.0f * std::log(u1));
            return {radius * std::cos(2.0f * Pi * u2), radius * std::sin(2.0f * Pi * u2)};
        }

        // Spectre JONSWAP S(omega) (m² s) d'une mer levée par un vent `wind` (m/s) sur `fetch` (m).
        float Jonswap(float omega, float wind, float fetch, float peak) {
            const float alpha = 0.076f * std::pow(wind * wind / (fetch * OceanGravity), 0.22f);
            const float sigma = omega <= peak ? 0.07f : 0.09f;
            const float r = std::exp(-(omega - peak) * (omega - peak) / (2.0f * sigma * sigma * peak * peak));
            const float ratio = peak / omega;
            return alpha * OceanGravity * OceanGravity / std::pow(omega, 5.0f) * std::exp(-1.25f * ratio * ratio * ratio * ratio)
                   * std::pow(3.3f, r);
        }

        // Part de l'énergie qui va dans la direction `theta` (radians depuis le vent) : cos(theta / 2)^(2 s) normalisé
        // (somme 1 sur le cercle), s d'après Mitsuyasu (1975) : les ondes du pic suivent le vent, les autres s'en écartent.
        float Spreading(float theta, float omega, float peak, float spread) {
            const float ratio = omega / peak;
            const float mitsuyasu = omega <= peak ? 6.97f * std::pow(ratio, 5.0f) : 9.77f * std::pow(ratio, -2.5f);
            // Au moins cos² : sans ce plancher, les vaguelettes partaient dans tous les sens (un bruit sans vent).
            const float s = std::clamp(std::max(mitsuyasu, 1.0f) / std::max(spread * spread, 0.01f), 0.05f, 60.0f);
            const float norm = std::exp(std::lgamma(s + 1.0f) - std::lgamma(s + 0.5f)) / (2.0f * std::sqrt(Pi));
            return norm * std::pow(std::abs(std::cos(0.5f * theta)), 2.0f * s);
        }

        // Énergie par unité de surface du plan des nombres d'onde (m⁴) de l'onde k, avant le tirage au hasard.
        float WaveEnergy(const OceanWaveSettings& settings, float kx, float kz) {
            const float k = std::sqrt(kx * kx + kz * kz);
            if (k <= 0.0f) {
                return 0.0f;
            }
            const float wind = std::max(settings.windSpeed, 0.5f);
            const float fetch = std::max(settings.fetch, 1000.0f);
            const float peak = 22.0f * std::cbrt(OceanGravity * OceanGravity / (wind * fetch));
            const float omega = std::sqrt(OceanGravity * k);
            const float derivative = OceanGravity / (2.0f * omega); // d omega / d k
            const float theta = std::atan2(kz, kx) - settings.windDirection * (Pi / 180.0f);
            const float fade = std::exp(-k * k * ShortWaveLength * ShortWaveLength);
            return Jonswap(omega, wind, fetch, peak) * Spreading(theta, omega, peak, settings.spread) * derivative / k * fade;
        }

        float CellArea(std::uint32_t cascade) {
            const float step = 2.0f * Pi / OceanCascadeLengths[cascade];
            return step * step;
        }

        bool InBand(std::uint32_t cascade, float kx, float kz) {
            const std::array<float, 2> band = OceanCascadeBand(cascade);
            const float k = std::sqrt(kx * kx + kz * kz);
            return k >= band[0] && k < band[1];
        }
    }

    std::array<float, 2> OceanCascadeBand(std::uint32_t cascade) {
        const float low = cascade == 0 ? 0.0f : 2.0f * Pi / OceanCascadeLengths[cascade] * BandOverlap;
        const float high = cascade + 1 < OceanCascadeCount ? 2.0f * Pi / OceanCascadeLengths[cascade + 1] * BandOverlap
                                                           : std::numeric_limits<float>::max();
        return {low, high};
    }

    std::array<float, 2> OceanWaveVector(std::uint32_t cascade, std::uint32_t m, std::uint32_t n) {
        const float step = 2.0f * Pi / OceanCascadeLengths[cascade];
        const float half = static_cast<float>(OceanWaveSize / 2);
        return {(static_cast<float>(m) - half) * step, (static_cast<float>(n) - half) * step};
    }

    float OceanAngularSpeed(float k) {
        const float base = 2.0f * Pi / OceanRepeatPeriod;
        return std::floor(std::sqrt(OceanGravity * k) / base) * base;
    }

    OceanSpectrum BuildOceanSpectrum(const OceanWaveSettings& settings, std::uint32_t cascade) {
        constexpr std::uint32_t N = OceanWaveSize;
        const float area = CellArea(cascade);
        std::vector<std::array<float, 2>> h0(std::size_t{N} * N);
        for (std::uint32_t n = 0; n < N; ++n) {
            for (std::uint32_t m = 0; m < N; ++m) {
                const auto [kx, kz] = OceanWaveVector(cascade, m, n);
                // Ligne et colonne 0 (k = -N/2) : sans opposée dans la grille (-k serait N/2), la hauteur ne serait pas réelle.
                if (m == 0 || n == 0 || !InBand(cascade, kx, kz)) {
                    h0[n * N + m] = {0.0f, 0.0f};
                    continue;
                }
                // E|h0|² = énergie x surface / 2 : la hauteur, somme des deux termes, a pour variance énergie x surface.
                const float amplitude = 0.5f * std::sqrt(WaveEnergy(settings, kx, kz) * area);
                const auto [a, b] = Gaussian(settings.seed, cascade, m, n);
                h0[n * N + m] = {a * amplitude, b * amplitude};
            }
        }
        OceanSpectrum spectrum(std::size_t{N} * N);
        for (std::uint32_t n = 0; n < N; ++n) {
            for (std::uint32_t m = 0; m < N; ++m) {
                // -k est l'onde (N - m, N - n).
                std::array<float, 2> opposite{0.0f, 0.0f};
                if (m > 0 && n > 0) {
                    opposite = h0[(N - n) * N + (N - m)];
                }
                const std::array<float, 2>& wave = h0[n * N + m];
                spectrum[n * N + m] = {wave[0], wave[1], opposite[0], -opposite[1]};
            }
        }
        return spectrum;
    }

    float OceanHeightDeviation(const OceanWaveSettings& settings) {
        double variance = 0.0;
        for (std::uint32_t cascade = 0; cascade < OceanCascadeCount; ++cascade) {
            const float area = CellArea(cascade);
            for (std::uint32_t n = 1; n < OceanWaveSize; ++n) {
                for (std::uint32_t m = 1; m < OceanWaveSize; ++m) {
                    const auto [kx, kz] = OceanWaveVector(cascade, m, n);
                    if (InBand(cascade, kx, kz)) {
                        variance += WaveEnergy(settings, kx, kz) * area;
                    }
                }
            }
        }
        return static_cast<float>(std::sqrt(variance));
    }
}
