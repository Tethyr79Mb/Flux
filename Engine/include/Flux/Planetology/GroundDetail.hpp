#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Flux {
    // Détail du sol des planètes (réglage du joueur et de l'éditeur). Une tuile est coupée en 4 quand la caméra est à
    // moins de GetSplitFactor fois la longueur de son bord : au plus loin, deux sommets voisins sont à environ 10 / 6,5 /
    // 5 pixels l'un de l'autre (1080 lignes, champ de 60°). Le nombre de tuiles croît comme le carré du facteur (x2,25 en
    // Élevé, x4 en Très élevé par rapport à Normal).
    enum class GroundDetail : std::uint8_t { Normal, High, VeryHigh };
    constexpr GroundDetail DefaultGroundDetail = GroundDetail::High;
    constexpr std::size_t GroundDetailCount = 3;
    // Dans l'ordre de l'énumération : nom dans les fichiers de réglages, nom à l'écran.
    constexpr std::array<std::string_view, GroundDetailCount> GroundDetailKeys{"normal", "high", "very-high"};
    constexpr std::array<std::string_view, GroundDetailCount> GroundDetailLabels{"Normal", "Élevé", "Très élevé"};

    constexpr double GetSplitFactor(GroundDetail detail) {
        switch (detail) {
            case GroundDetail::Normal: return 3.0;
            case GroundDetail::VeryHigh: return 6.0;
            case GroundDetail::High: break;
        }
        return 4.5;
    }
}
