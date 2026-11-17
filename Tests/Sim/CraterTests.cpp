// Cratères des planètes (CraterComponent, Terrain::Craters) : sans eux le relief ne change pas ; avec eux, des cuvettes
// et des rebords dans les bornes annoncées, sans marche, et les petits n'apparaissent qu'au détail fin.
#include "Check.hpp"

#include "Flux/Planetology/Planet.hpp"
#include "Flux/Scene/Components.hpp"

#include <algorithm>
#include <cmath>
#include <random>

using namespace Flux;

bool RunCraterTests() {
    std::mt19937 random(11);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    const auto randomDirection = [&] { return Math::Normalize(Math::Vector3d{unit(random), unit(random), unit(random)}); };

    // Densité 0 (ou pas de composant) : le relief d'origine, au bit près, mêmes octaves.
    const PlanetComponent planet{.radius = 200000.0f, .relief = 4500.0f, .seed = 3};
    const CraterComponent none{.density = 0.0f};
    const Planetology::Terrain plain(planet);
    const Planetology::Terrain empty(planet, &none);
    CHECK(Planetology::GetReliefShape(planet, &none) == Planetology::GetReliefShape(planet));
    CHECK(empty.GetCraterOctaves() == 0 && empty.GetCraterDepth() == 0.0 && empty.GetCraterShift() == 0);
    for (std::uint32_t level = 0; level <= plain.GetMaxLevel(); ++level) {
        CHECK(plain.GetOctaves(level) == empty.GetOctaves(level));
    }
    for (int i = 0; i < 100; ++i) {
        const Math::Vector3d direction = randomDirection();
        CHECK(plain.GetHeight(direction, 30) == empty.GetHeight(direction, 30));
    }

    // Une lune sans autre relief que ses cratères (réglages par défaut) : les plus grands ramenés à 12 % de son rayon
    // (6 km), puis 9 tailles jusqu'à 15 m environ.
    const PlanetComponent moon{.radius = 50000.0f, .relief = 0.0f, .seed = 9};
    const CraterComponent craters{};
    const Planetology::Terrain terrain(moon, &craters);
    CHECK(terrain.GetCraterScale() == 1.0 && terrain.GetCraterOctaves() == 9);
    CHECK(terrain.GetCraterDepth() > 0.0 && terrain.GetCraterRise() > 0.0);
    CHECK(Planetology::GetCraterDepth(moon, &craters) == terrain.GetCraterDepth());
    CHECK(Planetology::GetCraterDepth(moon, nullptr) == 0.0);
    // Le plus grand cratère possible : 12 % du rayon au plus.
    const CraterComponent huge{.maxRadius = 1.0e6f};
    CHECK(Planetology::Terrain(moon, &huge).GetCraterScale() == 1.0);

    // Cuvettes et rebords, dans les bornes ; une bonne part de la surface touchée.
    const std::uint32_t fine = terrain.GetOctaves(terrain.GetMaxLevel());
    double lowest = 0.0, highest = 0.0;
    int touched = 0;
    constexpr int Samples = 4000;
    for (int i = 0; i < Samples; ++i) {
        const double height = terrain.GetHeight(randomDirection(), fine);
        CHECK(height >= -terrain.GetCraterDepth() && height <= terrain.GetCraterRise());
        lowest = std::min(lowest, height);
        highest = std::max(highest, height);
        touched += height != 0.0 ? 1 : 0;
    }
    CHECK(lowest < -300.0);  // au fond d'un grand cratère (jusqu'à 1,4 km sous le sol pour 6 km de rayon)
    CHECK(highest > 20.0);   // sur un rebord
    CHECK(touched > Samples / 2);

    // Sans marche : le long d'une ligne de 2 km au pas de 0,25 m, la hauteur ne saute jamais (pentes des parois au plus
    // de l'ordre de 1).
    const Math::Vector3d start = randomDirection();
    const Math::Vector3d east = Math::Normalize(Math::Cross(start, Math::Vector3d{0.0, 1.0, 0.0}));
    double previous = terrain.GetHeight(start, fine);
    double change = 0.0;
    for (int step = 1; step <= 8000; ++step) {
        const double angle = 0.25 * step / moon.radius;
        const double height = terrain.GetHeight(start * std::cos(angle) + east * std::sin(angle), fine);
        change = std::max(change, std::abs(height - previous));
        previous = height;
    }
    CHECK(change < 0.5);

    // Les petits cratères n'apparaissent qu'au détail fin : une tuile grossière ne voit que les grands.
    const std::uint32_t coarse = terrain.GetOctaves(4);
    CHECK(coarse < terrain.GetCraterShift() + terrain.GetCraterOctaves());
    int differs = 0;
    for (int i = 0; i < 200; ++i) {
        const Math::Vector3d direction = randomDirection();
        differs += terrain.GetHeight(direction, coarse) != terrain.GetHeight(direction, fine) ? 1 : 0;
    }
    CHECK(differs > 0);
    // Ce qu'une tuile omet reste sous la profondeur de ses jupes (GetOmittedRelief).
    CHECK(terrain.GetOmittedRelief(coarse) > terrain.GetOmittedRelief(fine));
    CHECK(terrain.GetOmittedRelief(fine) == 0.0);

    // Les cratères s'ajoutent au relief d'une planète qui en a un.
    const Planetology::Terrain both(planet, &craters);
    int changed = 0;
    for (int i = 0; i < 200; ++i) {
        const Math::Vector3d direction = randomDirection();
        changed += both.GetHeight(direction, 30) != plain.GetHeight(direction, 30) ? 1 : 0;
    }
    CHECK(changed > 50);
    return true;
}
