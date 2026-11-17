// Spectre des vagues de l'océan (Planetology::BuildOceanSpectrum) : cascades qui se relaient sans trou ni doublon,
// hauteur réelle (chaque onde a son opposée conjuguée), mer poussée par le vent, hauteur des vagues plausible et qui
// monte avec le vent, tirage fixé par la graine. La somme par le GPU est vérifiée dans Tests/src/OceanWavesGpuTests.cpp.
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
    return true;
}
