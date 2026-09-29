#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector2.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Flux {
    // Image décodée en mémoire : RGBA 8 bits par canal (sRGB, alpha non prémultiplié), lignes de haut en bas.
    struct ImageData
    {
        Math::Vector2i size;
        std::vector<std::uint8_t> rgba; // size.x * size.y * 4 octets
    };

    // Plus grand côté accepté : la limite garantie de Vulkan pour une texture 2D sur les GPU de bureau.
    inline constexpr int MaxImageSize = 16384;

    // Formats lus : PNG, JPEG, TGA, BMP. Les images en niveaux de gris ou sans alpha sont converties en RGBA.
    FLUX_API std::expected<ImageData, std::string> LoadImageFile(const std::filesystem::path& path);
    FLUX_API std::expected<ImageData, std::string> LoadImageMemory(std::span<const std::uint8_t> bytes);
}
