#pragma once

#include "Flux/SimExport.hpp"
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
    FLUX_SIM_API std::expected<ImageData, std::string> LoadImageFile(const std::filesystem::path& path);
    FLUX_SIM_API std::expected<ImageData, std::string> LoadImageMemory(std::span<const std::uint8_t> bytes);

    // Niveaux de mip d'une image de couleur sRGB (niveau 0 compris), à la suite, chacun moitié du précédent arrondi en
    // dessous jusqu'à 1 x 1, comme Vulkan les range ; levels reçoit leur nombre. Couleur moyennée en linéaire, pondérée
    // par l'alpha (les pixels transparents, souvent noirs, n'assombrissent pas les bords). alphaCutoff > 0 (matériau à
    // seuil, feuillage) : l'alpha de chaque niveau est mis à l'échelle pour qu'autant de pixels passent le seuil qu'au
    // niveau 0 (Castaño, 2010). Moyenné simplement, il baisse à chaque niveau et le feuillage s'évapore de loin.
    FLUX_SIM_API std::vector<std::uint8_t> BuildMipChain(const ImageData& image, float alphaCutoff, std::uint32_t& levels);

    // Image encodée en PNG, en mémoire (vide si l'image est vide ou invalide).
    FLUX_SIM_API std::vector<std::uint8_t> EncodePng(const ImageData& image);
}
