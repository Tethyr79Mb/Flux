#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Flux {
    // Vue de débogage du G-buffer des modèles opaques (panneau Moteur, outil debug_view de l'éditeur) : à la place de
    // leur éclairage, la couleur de base, les normales de la carte (xyz ramenés de [-1, 1] à [0, 1]) ou métal (rouge),
    // rugosité (vert) et occlusion (bleu). Le reste de l'image (sol, ciel, arbres) est dessiné comme d'habitude.
    enum class GBufferView : std::uint8_t { None, BaseColor, Normals, MetalRoughness };
    constexpr std::size_t GBufferViewCount = 4;
    // Dans l'ordre de l'énumération : nom pour l'outil de l'éditeur, nom à l'écran.
    constexpr std::array<std::string_view, GBufferViewCount> GBufferViewKeys{"none", "base_color", "normals", "metal_roughness"};
    constexpr std::array<std::string_view, GBufferViewCount> GBufferViewLabels{"Aucune", "Couleur de base", "Normales",
                                                                               "Métal, rugosité, occlusion"};
}
