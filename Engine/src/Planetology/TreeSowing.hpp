#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Planetology/Planet.hpp"
#include "Flux/Scene/Components.hpp"
#include "Planetology/VegetationLibrary.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace Flux::Planetology {
    // Places possibles d'arbres par côté d'une plaine (TreeGrid dans shaders/PlanetTile.slang).
    constexpr std::uint32_t TreeGrid = 64;
    // Niveau des tuiles qui servent de plaines (semées d'un coup) : le premier dont le bord fait au plus 400 m.
    FLUX_API std::uint32_t GetPlainLevel(const Terrain& terrain);

    // Réglages du semis tirés de la planète, les mêmes pour le GPU (VegetationRenderer::TreeJob) et le CPU (SowPlain).
    struct SowSettings
    {
        std::uint32_t seed = 0;         // hasard des places
        std::array<float, 4> shape{};   // rayon, relief, altitude de la neige, haut de la plage (m)
        std::array<float, 4> climate{}; // températures à l'équateur, aux pôles, humidité ; rayon de l'océan (0 : aucun)

        constexpr bool operator==(const SowSettings&) const = default;
    };
    FLUX_API SowSettings GetSowSettings(const Terrain& terrain, const PlanetComponent& planet);

    // Espacement des arbres : côté (en places de la grille, 1 à MaxTreeBlock) des blocs dont une seule place garde un
    // arbre, pour chaque espèce (4 bits chacune, dans l'ordre de VegetationSet::species) de chaque biome ; et les côtés
    // employés (bit k), qui écartent les autres places avant le relief. Calculé une fois par le CPU, pour le GPU
    // (SowVegetation) et SowPlain : le même arrondi des deux côtés.
    constexpr std::uint32_t MaxTreeBlock = 8;
    struct TreeBlocks
    {
        std::array<std::uint32_t, VegetationSet::MaxBiomes> sides{};
        std::uint32_t used = 0;
    };
    FLUX_API TreeBlocks GetTreeBlocks(const Terrain& terrain, const VegetationSet& set);

    struct SownTree
    {
        Math::Vector3d position;   // pied, repère de la planète (centre à l'origine) ; il pousse vers le haut (radial)
        float scale = 1.0f;        // x le modèle
        std::uint32_t species = 0; // dans VegetationSet::species
    };

    // Semis d'une plaine (tuile de niveau VegetationRenderer::GetPlainLevel) refait par le CPU, en double, pour la
    // collision des arbres : copie de SowPlace (shaders/PlanetTile.slang) avec ClimateAt, ForestNoise et BiomeFit
    // (shaders/PlanetClimate.slang), mêmes places, espèces et tailles que les arbres dessinés. Le relief du GPU étant
    // en float, seul un arbre au ras d'un seuil (plage, neige, pente, bord d'une forêt ou d'un biome) peut différer ;
    // VegetationGpuTests vérifie que les deux restent égaux. Toute modification va dans les deux.
    // Exporté pour les tests.
    FLUX_API void SowPlain(const Terrain& terrain, const SowSettings& settings, const VegetationSet& set, TileKey plain,
                           std::vector<SownTree>& trees);
}
