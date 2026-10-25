#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Flux {
    // Qualité des nuages volumétriques (réglage du joueur et de l'éditeur) : nombre de pas le long du rayon et vers le
    // soleil (shaders/Clouds.slang). Le coût de la marche suit à peu près le produit des deux : Bas en coûte environ
    // 40 %, Moyen 65 % de Élevé. Moins de pas : plus de grain, que l'historique efface moins bien en mouvement.
    enum class CloudQuality : std::uint8_t { Low, Medium, High };
    constexpr CloudQuality DefaultCloudQuality = CloudQuality::High;
    constexpr std::size_t CloudQualityCount = 3;
    // Dans l'ordre de l'énumération : nom dans les fichiers de réglages, nom à l'écran.
    constexpr std::array<std::string_view, CloudQualityCount> CloudQualityKeys{"low", "medium", "high"};
    constexpr std::array<std::string_view, CloudQualityCount> CloudQualityLabels{"Bas", "Moyen", "Élevé"};

    struct CloudSteps
    {
        std::uint32_t view;  // le long du rayon, au plus
        std::uint32_t light; // vers le soleil, là où il y a du nuage
    };

    constexpr CloudSteps GetCloudSteps(CloudQuality quality) {
        switch (quality) {
            case CloudQuality::Low: return {32, 4};
            case CloudQuality::Medium: return {48, 5};
            case CloudQuality::High: break;
        }
        return {64, 6};
    }
}
