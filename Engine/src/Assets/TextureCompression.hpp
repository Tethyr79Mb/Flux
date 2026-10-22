#pragma once

#include "Flux/Export.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Flux {
    // Préparation des textures compressées sur le CPU (sol des planètes) : niveaux de mip, que le GPU ne sait pas
    // réduire dans un format compressé, puis compression BC7 (bc7enc).

    // Niveaux de mip d'une image carrée RGBA8. Chaque niveau est la moyenne de 2 x 2 pixels du précédent ; srgb : moyenne des couleurs
    // en linéaire (comme le GPU sur une image sRGB), l'alpha reste linéaire.

    // Nombre de niveaux jusqu'à 1 x 1.
    FLUX_API std::uint32_t MipLevelCount(std::uint32_t size);

    // Tous les niveaux d'une image de size x size, à la suite (niveau 0 compris, recopié).
    FLUX_API std::vector<std::uint8_t> BuildMips(std::span<const std::uint8_t> rgba, std::uint32_t size, bool srgb);

    // Octets d'une image width x height en BC7 : un bloc de 16 octets par carré de 4 x 4 (bords complétés).
    FLUX_API std::size_t BC7Size(std::uint32_t width, std::uint32_t height);
    // Compresse une image RGBA8 en BC7 dans out (BC7Size octets). perceptual : erreur mesurée comme l'œil la voit
    // (couleurs) ; sinon chaque canal compte autant (normales). Plusieurs fils peuvent compresser à la fois.
    FLUX_API void CompressBC7(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height, bool perceptual,
                     std::span<std::uint8_t> out);
}
