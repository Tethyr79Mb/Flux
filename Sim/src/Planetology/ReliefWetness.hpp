#pragma once

#include "Flux/SimExport.hpp"
#include "Flux/Planetology/Planet.hpp"

#include <array>
#include <cstdint>

namespace Flux::Planetology {
    // Humidité du relief : l'eau descend et s'accumule dans les creux, les crêtes sèchent. Mesurée par la position du
    // point dans son relief (hauteur moyenne à WetnessReach autour moins la sienne, sur le relief grossier) : de -1
    // (crête, sol mince et sec) à 1 (fond de vallon, sol épais et humide). Une formule, rien de stocké : calculée sur
    // une petite grille par tuile (WetnessHeights dans shaders/PlanetTile.slang), puis interpolée. Sert au sol, à
    // l'herbe et aux arbres (humidité du climat et couverture des forêts) ; le semis du CPU (SowPlain) la refait en
    // double. Toute modification va dans les deux.
    constexpr double WetnessReach = 300.0;      // m, distance des quatre voisins comparés
    constexpr double WetnessCell = 150.0;       // m, écart visé entre deux points de la grille
    constexpr std::uint32_t MaxWetnessSide = 8; // mailles de la grille par côté de tuile, au plus
    constexpr double ReliefHumidity = 0.25;     // humidité ajoutée au fond d'un vallon (ReliefHumidity dans PlanetClimate.slang)
    constexpr double ReliefForest = 0.3;        // part couverte de forêts ajoutée (ReliefForest dans PlanetClimate.slang)

    // Grille d'une tuile de ce niveau : octaves du relief grossier, mailles par côté (puissance de 2, 0 : aucune) et
    // fondu (1 à 0) des tuiles trop grandes, où la grille ne verrait plus les vallons de WetnessReach.
    struct WetnessLayout
    {
        std::uint32_t octaves = 0;
        std::uint32_t side = 0;
        float fade = 0.0f;
    };
    FLUX_SIM_API WetnessLayout GetWetnessLayout(const Terrain& terrain, std::uint32_t level);

    struct WetnessGrid
    {
        WetnessLayout layout;
        std::array<double, (MaxWetnessSide + 1) * (MaxWetnessSide + 1)> values{}; // par ligne, fondu non compris
    };
    FLUX_SIM_API void BuildWetnessGrid(const Terrain& terrain, TileKey key, WetnessGrid& grid);
    // Humidité du relief au point (u, v) de la grille de la tuile (0 à TileQuads), fondu compris.
    FLUX_SIM_API double SampleWetness(const WetnessGrid& grid, double u, double v);
}
