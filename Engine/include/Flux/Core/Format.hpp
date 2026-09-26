#pragma once

#include <cmath>
#include <format>
#include <string>

namespace Flux {
    // Distance lisible : mètres, kilomètres, unités astronomiques puis années-lumière.
    inline std::string FormatDistance(double meters) {
        const double magnitude = std::abs(meters);
        if (magnitude < 1.0e4) {
            return std::format("{:.1f} m", meters);
        }
        if (magnitude < 1.0e9) {
            return std::format("{:.1f} km", meters / 1.0e3);
        }
        if (magnitude < 1.0e15) {
            return std::format("{:.3f} ua", meters / 1.495978707e11);
        }
        return std::format("{:.4f} al", meters / 9.4607304725808e15);
    }
}
