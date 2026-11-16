#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Flux {
    // Détail des modèles (réglage du joueur et de l'éditeur) : écart toléré à l'écran, en pixels, entre un niveau
    // simplifié d'un modèle (ModelData::lods) et le modèle complet. Le moteur prend le niveau le plus léger qui reste
    // sous cet écart : plus il est grand, plus les modèles se simplifient tôt.
    enum class ModelDetail : std::uint8_t { Low, Medium, High, Ultra };
    constexpr ModelDetail DefaultModelDetail = ModelDetail::High;
    constexpr std::size_t ModelDetailCount = 4;
    // Dans l'ordre de l'énumération : nom dans les fichiers de réglages, nom à l'écran.
    constexpr std::array<std::string_view, ModelDetailCount> ModelDetailKeys{"low", "medium", "high", "ultra"};
    constexpr std::array<std::string_view, ModelDetailCount> ModelDetailLabels{"Bas", "Moyen", "Élevé", "Ultra"};

    constexpr float GetModelDetailPixels(ModelDetail detail) {
        switch (detail) {
            case ModelDetail::Low: return 4.0f;
            case ModelDetail::Medium: return 2.0f;
            case ModelDetail::High: break;
            case ModelDetail::Ultra: return 0.5f;
        }
        return 1.0f;
    }
}
