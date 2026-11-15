#include "Planetology/ReliefWetness.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace Flux::Planetology {
    namespace {
        // Position du point dans son relief (WetnessAt dans shaders/PlanetTile.slang) : les quatre voisins à
        // WetnessReach le long de l'axe s de la face et de sa perpendiculaire, dans le plan tangent.
        double WetnessAt(const Terrain& terrain, const Math::Vector3d& d, const Math::Vector3d& axisS, std::uint32_t octaves) {
            const double epsilon = WetnessReach / terrain.GetRadius();
            const double scale = 1.0 / std::sqrt(1.0 + epsilon * epsilon);
            const Math::Vector3d t1 = Math::Normalize(axisS - d * Math::Dot(d, axisS));
            const Math::Vector3d t2 = Math::Cross(d, t1);
            double around = 0.0;
            for (std::uint32_t k = 0; k < 4; ++k) {
                const Math::Vector3d t = (k < 2 ? t1 : t2) * (k % 2 == 0 ? epsilon : -epsilon);
                around += terrain.GetHeight((d + t) * scale, octaves);
            }
            const double hollow = 0.25 * around - terrain.GetHeight(d, octaves);
            const double depth = std::max(0.01 * terrain.GetRelief(), 2.0);
            return hollow / (std::abs(hollow) + depth);
        }
    }

    WetnessLayout GetWetnessLayout(const Terrain& terrain, std::uint32_t level) {
        const double R = terrain.GetRadius();
        // Bord d'une tuile de niveau n : environ (π/2) R / 2^n.
        const double edge = 0.5 * std::numbers::pi * R / std::ldexp(1.0, static_cast<int>(level));
        WetnessLayout layout;
        const auto wanted = static_cast<std::uint32_t>(std::clamp(std::ceil(edge / WetnessCell), 1.0, static_cast<double>(MaxWetnessSide)));
        const std::uint32_t side = std::bit_ceil(wanted);
        // Pleine tant que la maille fait au plus deux fois WetnessReach, nulle à quatre fois.
        const double spacing = edge / side;
        layout.fade = static_cast<float>(std::clamp(2.0 - spacing / (2.0 * WetnessReach), 0.0, 1.0));
        if (layout.fade <= 0.0f) {
            return layout;
        }
        layout.side = side;
        // Relief grossier : les octaves d'une tuile dont les sommets sont espacés d'environ WetnessReach / 4 (le détail
        // fin, rochers et bosses, ne fait pas de vallon).
        const double fine = (0.5 * std::numbers::pi * R / TileQuads) / (0.25 * WetnessReach);
        const auto coarseLevel = static_cast<std::uint32_t>(std::clamp(std::ceil(std::log2(std::max(fine, 1.0))), 0.0,
                                                                       static_cast<double>(terrain.GetMaxLevel())));
        layout.octaves = terrain.GetOctaves(coarseLevel);
        return layout;
    }

    void BuildWetnessGrid(const Terrain& terrain, TileKey key, WetnessGrid& grid) {
        grid.layout = GetWetnessLayout(terrain, key.level);
        const std::uint32_t side = grid.layout.side;
        if (side == 0) {
            return;
        }
        const Math::Vector3d axisS = GetFaceAxisS(key.face);
        for (std::uint32_t b = 0; b <= side; ++b) {
            for (std::uint32_t a = 0; a <= side; ++a) {
                const Math::Vector3d d = DirectionInTile(key, static_cast<double>(a * (TileQuads / side)),
                                                         static_cast<double>(b * (TileQuads / side)));
                grid.values[b * (side + 1) + a] = WetnessAt(terrain, d, axisS, grid.layout.octaves);
            }
        }
    }

    double SampleWetness(const WetnessGrid& grid, double u, double v) {
        const std::uint32_t side = grid.layout.side;
        if (side == 0) {
            return 0.0;
        }
        const double x = std::clamp(u, 0.0, static_cast<double>(TileQuads)) * side / TileQuads;
        const double y = std::clamp(v, 0.0, static_cast<double>(TileQuads)) * side / TileQuads;
        const std::uint32_t a = std::min(static_cast<std::uint32_t>(x), side - 1);
        const std::uint32_t b = std::min(static_cast<std::uint32_t>(y), side - 1);
        const double fx = x - a, fy = y - b;
        const auto at = [&](std::uint32_t i, std::uint32_t j) { return grid.values[j * (side + 1) + i]; };
        const double low = at(a, b) + (at(a + 1, b) - at(a, b)) * fx;
        const double high = at(a, b + 1) + (at(a + 1, b + 1) - at(a, b + 1)) * fx;
        return (low + (high - low) * fy) * grid.layout.fade;
    }
}
