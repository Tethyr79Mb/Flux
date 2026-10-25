#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Flux {
    // Détail du sol des planètes (réglage du joueur et de l'éditeur). Une tuile est coupée en 4 quand la caméra est à
    // moins de GetSplitFactor fois la longueur de son bord : au plus loin, deux sommets voisins sont à environ 10 / 7,5
    // / 6 / 5 pixels l'un de l'autre (1080 lignes, champ de 60°). Le nombre de tuiles croît comme le carré du facteur
    // (x1,8 en Moyen, x2,8 en Élevé, x4 en Ultra par rapport à Bas).
    enum class GroundDetail : std::uint8_t { Low, Medium, High, Ultra };
    constexpr GroundDetail DefaultGroundDetail = GroundDetail::High;
    constexpr std::size_t GroundDetailCount = 4;
    // Dans l'ordre de l'énumération : nom dans les fichiers de réglages, nom à l'écran.
    constexpr std::array<std::string_view, GroundDetailCount> GroundDetailKeys{"low", "medium", "high", "ultra"};
    constexpr std::array<std::string_view, GroundDetailCount> GroundDetailLabels{"Bas", "Moyen", "Élevé", "Ultra"};

    constexpr double GetSplitFactor(GroundDetail detail) {
        switch (detail) {
            case GroundDetail::Low: return 3.0;
            case GroundDetail::Medium: return 4.0;
            case GroundDetail::Ultra: return 6.0;
            case GroundDetail::High: break;
        }
        return 5.0;
    }
}
