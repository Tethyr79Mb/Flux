#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Flux {
    // Occlusion ambiante GTAO (réglage du joueur et de l'éditeur) : coins, dessous et creux privés d'une partie de la
    // lumière ambiante. Haute : pleine taille de rendu (configuration recommandée) ; Moyenne : un pixel sur quatre,
    // agrandi en suivant les bords (configuration minimum).
    enum class AmbientOcclusionQuality : std::uint8_t { Off, Medium, High };
    constexpr AmbientOcclusionQuality DefaultAmbientOcclusionQuality = AmbientOcclusionQuality::High;
    constexpr std::size_t AmbientOcclusionQualityCount = 3;
    // Dans l'ordre de l'énumération : nom dans les fichiers de réglages, nom à l'écran.
    constexpr std::array<std::string_view, AmbientOcclusionQualityCount> AmbientOcclusionQualityKeys{"off", "medium", "high"};
    constexpr std::array<std::string_view, AmbientOcclusionQualityCount> AmbientOcclusionQualityLabels{"Désactivée", "Moyenne",
                                                                                                       "Haute"};
}
