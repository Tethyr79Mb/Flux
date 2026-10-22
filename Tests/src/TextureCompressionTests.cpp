// Textures compressées du sol des planètes : mips calculés sur le CPU et compression BC7 (bc7enc), relue par le
// décodeur de bc7enc.
#include "Check.hpp"

#include "Assets/TextureCompression.hpp"

#include <bc7decomp.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace Flux;

bool RunTextureCompressionTests() {
    CHECK(MipLevelCount(2048) == 12 && MipLevelCount(1) == 1 && MipLevelCount(4) == 3);
    CHECK(BC7Size(2048, 2048) == 512u * 512u * 16u && BC7Size(2, 1) == 16 && BC7Size(5, 4) == 32);

    // 2 x 2 : noir et blanc en damier, alpha 0 et 255. Moyenne linéaire (128), ou en lumière pour le sRGB (188).
    const std::vector<std::uint8_t> checker{0, 0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0};
    const std::vector<std::uint8_t> linear = BuildMips(checker, 2, false);
    CHECK(linear.size() == (4 + 1) * 4);
    CHECK(linear[16] == 128 && linear[19] == 128);
    const std::vector<std::uint8_t> srgb = BuildMips(checker, 2, true);
    CHECK(srgb[16] == 188 && srgb[17] == 188 && srgb[19] == 128); // l'alpha reste linéaire

    // Image de 16 x 16 en dégradés et bruit (comme une texture de sol) : relue à moins de 3 niveaux d'écart en moyenne.
    constexpr std::uint32_t Size = 16;
    std::vector<std::uint8_t> image(Size * Size * 4);
    std::uint32_t seed = 12345;
    for (std::uint32_t i = 0; i < Size * Size; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const std::uint32_t noise = (seed >> 24) % 24;
        image[i * 4] = static_cast<std::uint8_t>(80 + (i % Size) * 6 + noise);
        image[i * 4 + 1] = static_cast<std::uint8_t>(60 + (i / Size) * 5 + noise);
        image[i * 4 + 2] = static_cast<std::uint8_t>(40 + noise);
        image[i * 4 + 3] = static_cast<std::uint8_t>(120 + (i % Size) * 4);
    }
    std::vector<std::uint8_t> blocks(BC7Size(Size, Size));
    CompressBC7(image, Size, Size, true, blocks);
    double error = 0.0;
    for (std::uint32_t by = 0; by < Size / 4; ++by) {
        for (std::uint32_t bx = 0; bx < Size / 4; ++bx) {
            bc7decomp::color_rgba pixels[16];
            CHECK(bc7decomp::unpack_bc7(blocks.data() + (by * (Size / 4) + bx) * 16, pixels));
            for (std::uint32_t p = 0; p < 16; ++p) {
                const std::uint32_t at = ((by * 4 + p / 4) * Size + bx * 4 + p % 4) * 4;
                for (std::uint32_t c = 0; c < 4; ++c) {
                    error += std::abs(static_cast<int>(pixels[p].m_comps[c]) - static_cast<int>(image[at + c]));
                }
            }
        }
    }
    CHECK(error / (Size * Size * 4) < 3.0);

    // Plus petite que 4 x 4 (derniers mips) : un bloc entier, les pixels qui manquent répètent le bord.
    const std::vector<std::uint8_t> tiny{200, 100, 50, 255};
    std::vector<std::uint8_t> block(BC7Size(1, 1));
    CompressBC7(tiny, 1, 1, true, block);
    bc7decomp::color_rgba pixels[16];
    CHECK(bc7decomp::unpack_bc7(block.data(), pixels));
    CHECK(std::abs(pixels[15].m_comps[0] - 200) <= 2 && std::abs(pixels[15].m_comps[2] - 50) <= 2);
    return true;
}
