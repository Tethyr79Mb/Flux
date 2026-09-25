#pragma once

#include "Flux/Math/Vector3.hpp"

#include <cmath>
#include <cstdint>

namespace Flux::Math {
    // Position à l'échelle galaxie : un secteur entier (cube de 1 million de km) plus un décalage en double.
    // Le décalage reste dans [-SectorSize / 2, SectorSize / 2] : sa précision ne descend jamais sous le micromètre.
    struct WorldPosition
    {
        static constexpr double SectorSize = 1.0e9; // mètres

        Vector3i64 sector;
        Vector3d offset;

        constexpr bool operator==(const WorldPosition&) const = default;

        // Reporte les secteurs entiers du décalage dans sector.
        void Normalize() {
            const auto carry = [](std::int64_t& sectorAxis, double& offsetAxis) {
                const double whole = std::round(offsetAxis / SectorSize);
                sectorAxis += static_cast<std::int64_t>(whole);
                offsetAxis -= whole * SectorSize;
            };
            carry(sector.x, offset.x);
            carry(sector.y, offset.y);
            carry(sector.z, offset.z);
        }

        WorldPosition& operator+=(const Vector3d& delta) {
            offset += delta;
            Normalize();
            return *this;
        }
    };

    // Écart to - from en mètres. Exact au micromètre près quand les deux positions sont proches ;
    // au-delà, l'erreur reste négligeable devant la distance (objets à peine visibles).
    inline Vector3d Delta(const WorldPosition& from, const WorldPosition& to) {
        const Vector3d sectors{static_cast<double>(to.sector.x - from.sector.x),
                               static_cast<double>(to.sector.y - from.sector.y),
                               static_cast<double>(to.sector.z - from.sector.z)};
        return sectors * WorldPosition::SectorSize + (to.offset - from.offset);
    }
}
