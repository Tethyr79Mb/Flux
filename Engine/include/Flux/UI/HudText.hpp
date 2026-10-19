#pragma once

#include <algorithm>
#include <format>
#include <string>

namespace Flux {
    // Distance à la française pour l'écran du joueur : en m, en km, en millions de km.
    inline std::string FormatDistance(double meters) {
        std::string text = meters >= 1.0e9   ? std::format("{:.1f} millions de km", meters / 1.0e9)
                           : meters >= 1.0e4 ? std::format("{:.1f} km", meters / 1000.0)
                                             : std::format("{:.0f} m", meters);
        std::ranges::replace(text, '.', ',');
        return text;
    }
}
