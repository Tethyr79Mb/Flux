#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Flux {
    // Distance d'affichage des arbres (réglage du joueur et de l'éditeur) : facteur de la portée de chaque planète
    // (VegetationComponent::range, le réglage artistique). Les plaines d'arbres suivent le carré de la distance : Ultra
    // en demande environ 2,25 fois plus qu'Élevé, et au-delà de VegetationRenderer::MaxVisiblePlains les plus
    // lointaines restent sans arbres.
    enum class VegetationDistance : std::uint8_t { Low, Medium, High, Ultra };
    constexpr VegetationDistance DefaultVegetationDistance = VegetationDistance::High;
    constexpr std::size_t VegetationDistanceCount = 4;
    // Dans l'ordre de l'énumération : nom dans les fichiers de réglages, nom à l'écran.
    constexpr std::array<std::string_view, VegetationDistanceCount> VegetationDistanceKeys{"low", "medium", "high", "ultra"};
    constexpr std::array<std::string_view, VegetationDistanceCount> VegetationDistanceLabels{"Bas", "Moyen", "Élevé", "Ultra"};

    constexpr float GetVegetationDistanceScale(VegetationDistance distance) {
        switch (distance) {
            case VegetationDistance::Low: return 0.5f;
            case VegetationDistance::Medium: return 0.75f;
            case VegetationDistance::High: break;
            case VegetationDistance::Ultra: return 1.5f;
        }
        return 1.0f;
    }
}
