// Spectre des vagues de l'océan (Planetology::BuildOceanSpectrum) : cascades qui se relaient sans trou ni doublon,
// hauteur réelle (chaque onde a son opposée conjuguée), mer poussée par le vent, hauteur des vagues plausible et qui
// monte avec le vent, tirage fixé par la graine ; plan des vagues qui suit la caméra sans faire glisser l'eau. La somme par le GPU est vérifiée dans Tests/src/OceanWavesGpuTests.cpp.
#include "Check.hpp"

#include "Flux/Planetology/OceanWaves.hpp"

#include <algorithm>
#include <cmath>

using namespace Flux::Planetology;

namespace {
    constexpr std::uint32_t N = OceanWaveSize;

    float Energy(const std::array<float, 4>& entry) {
        return entry[0] * entry[0] + entry[1] * entry[1];
    }

    // La caméra, à 20 m au-dessus d'une mer de `radius` m, part de `start` et suit un grand cercle (pas de 5 m) vers
    // `toward`. Le point de la mer passé à mi-chemin doit garder ses coordonnées dans le plan des vagues de 1 km avant
    // lui à 1 km après (à 1 cm près).
    bool PlaneHoldsWater(double radius, const Flux::Math::Vector3d& start, const Flux::Math::Vector3d& toward) {
        using Flux::Math::Vector3d;
        const Vector3d a = Flux::Math::Normalize(start);
        const Vector3d b = Flux::Math::Normalize(toward - a * Flux::Math::Dot(toward, a));
        const auto at = [&](double metres, double height) {
            const double angle = metres / radius;
            return (a * std::cos(angle) + b * std::sin(angle)) * (radius + height);
        };
        const Vector3d water = at(10000.0, 0.0);
        OceanWavePlane plane;
        std::array<double, 2> before{};
        std::array<double, 2> after{};
        for (double metres = 0.0; metres <= 11000.0; metres += 5.0) {
            plane.Follow(at(metres, 20.0), radius);
            if (metres == 9000.0) {
                before = plane.GetCoordinates(water);
            }
            if (metres == 11000.0) {
                after = plane.GetCoordinates(water);
            }
        }
        const double slip = std::hypot(after[0] - before[0], after[1] - before[1]);
        std::printf("       eau qui glisse sur 2 km (rayon %.0f km) : %.4f m\n", radius * 0.001, slip);
        return slip < 0.01 && std::abs(Flux::Math::Length(plane.GetAnchor()) - radius) < 1.0e-6 * radius;
    }
}

bool RunOceanWavesTests() {
    for (std::uint32_t cascade = 0; cascade + 1 < OceanCascadeCount; ++cascade) {
        CHECK(OceanCascadeBand(cascade)[1] == OceanCascadeBand(cascade + 1)[0]);
        CHECK(OceanCascadeBand(cascade)[0] < OceanCascadeBand(cascade)[1]);
    }
    CHECK(OceanCascadeBand(0)[0] == 0.0f);

    const OceanWaveSettings settings;
    const float deviation = OceanHeightDeviation(settings);
    std::printf("       vent 8 m/s sur 100 km : hauteur significative %.2f m\n", 4.0f * deviation);
    // Mer de force 4 à 5 : vagues de 1 à 2,5 m.
    CHECK(4.0f * deviation > 1.0f && 4.0f * deviation < 2.5f);
    OceanWaveSettings windy = settings;
    windy.windSpeed = 15.0f;
    // À longueur de mer donnée, la hauteur suit à peu près le vent (JONSWAP).
    CHECK(OceanHeightDeviation(windy) > 1.5f * deviation);

    double variance = 0.0;
    for (std::uint32_t cascade = 0; cascade < OceanCascadeCount; ++cascade) {
        const OceanSpectrum spectrum = BuildOceanSpectrum(settings, cascade);
        CHECK(spectrum.size() == std::size_t{N} * N);
        double downwind = 0.0;
        double upwind = 0.0;
        for (std::uint32_t n = 1; n < N; ++n) {
            for (std::uint32_t m = 1; m < N; ++m) {
                const std::array<float, 4>& entry = spectrum[n * N + m];
                const std::array<float, 4>& opposite = spectrum[(N - n) * N + (N - m)];
                CHECK(entry[2] == opposite[0] && entry[3] == -opposite[1]);
                variance += 2.0 * Energy(entry);
                // Vent vers +x par défaut : les ondes avancent dans le sens de k.
                (OceanWaveVector(cascade, m, n)[0] > 0.0f ? downwind : upwind) += Energy(entry);
            }
        }
        std::printf("       cascade %u : énergie sous le vent / contre le vent %.1f\n", cascade,
                    downwind / std::max(upwind, 1e-30));
        // La houle suit le vent ; les vaguelettes s'en écartent davantage (Mitsuyasu), sans le remonter.
        CHECK(downwind > (cascade == 0 ? 3.0 : 1.2) * upwind);
    }
    // Le tirage au hasard s'écarte un peu de l'attendu, pas du simple au double.
    const double ratio = std::sqrt(variance) / deviation;
    std::printf("       écart type tiré / attendu : %.2f\n", ratio);
    CHECK(ratio > 0.7 && ratio < 1.3);

    OceanWaveSettings other = settings;
    other.seed = 7;
    CHECK(BuildOceanSpectrum(settings, 0) == BuildOceanSpectrum(settings, 0));
    CHECK(BuildOceanSpectrum(other, 0) != BuildOceanSpectrum(settings, 0));

    // Plan des vagues : vers l'est, en biais, par-dessus un pôle, sur une petite lune.
    CHECK(PlaneHoldsWater(6.0e6, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}));
    CHECK(PlaneHoldsWater(6.0e6, {1.0, 0.3, 0.2}, {-0.2, 1.0, 0.4}));
    CHECK(PlaneHoldsWater(6.0e6, {0.0, 1.0, 0.001}, {1.0, 0.0, 0.0}));
    CHECK(PlaneHoldsWater(2.0e5, {0.3, -0.5, 0.8}, {1.0, 0.2, 0.0}));
    {
        OceanWavePlane plane;
        plane.Follow({6.0e6, 0.0, 0.0}, 6.0e6);
        const auto& east = plane.GetEast();
        const auto& north = plane.GetNorth();
        CHECK(std::abs(Flux::Math::Dot(east, north)) < 1.0e-12 && std::abs(Flux::Math::Dot(east, plane.GetUp())) < 1.0e-12);
        CHECK(north.y > 0.999); // axe z vers le pôle nord (y de la planète)
        plane.Follow({6.0e6, 100.0, 0.0}, 6.0e6);
        CHECK(std::abs(plane.GetOffset()[1] - 100.0) < 0.01);
        plane.Follow({6.0e6, 2.0e5, 0.0}, 6.0e6); // saut de 200 km : plan reposé
        CHECK(plane.GetOffset()[0] == 0.0 && plane.GetOffset()[1] == 0.0);
    }
    return true;
}
