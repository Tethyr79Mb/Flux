#include "Assets/TextureCompression.hpp"

#include <bc7enc.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>

namespace Flux {
    namespace {
        // sRGB vers linéaire sur 16 bits (précision gardée pour les sombres), et retour par recherche.
        struct SrgbTables
        {
            std::array<std::uint16_t, 256> toLinear{};
            SrgbTables() {
                for (int i = 0; i < 256; ++i) {
                    const float c = static_cast<float>(i) / 255.0f;
                    const float linear = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
                    toLinear[i] = static_cast<std::uint16_t>(std::lround(linear * 65535.0f));
                }
            }
            std::uint8_t ToSrgb(std::uint32_t linear) const {
                // La valeur sRGB dont le linéaire est le plus proche (tableau croissant).
                const auto upper = std::lower_bound(toLinear.begin(), toLinear.end(), linear);
                if (upper == toLinear.begin()) {
                    return 0;
                }
                if (upper == toLinear.end()) {
                    return 255;
                }
                const auto lower = upper - 1;
                return static_cast<std::uint8_t>((linear - *lower <= *upper - linear ? lower : upper) - toLinear.begin());
            }
        };

        const SrgbTables& Tables() {
            static const SrgbTables tables;
            return tables;
        }

        void Halve(const std::uint8_t* in, std::uint32_t size, std::uint8_t* out, bool srgb) {
            const std::uint32_t half = size / 2;
            const SrgbTables& tables = Tables();
            for (std::uint32_t y = 0; y < half; ++y) {
                for (std::uint32_t x = 0; x < half; ++x) {
                    const std::uint8_t* p[4] = {in + (static_cast<std::size_t>(2 * y) * size + 2 * x) * 4,
                                                in + (static_cast<std::size_t>(2 * y) * size + 2 * x + 1) * 4,
                                                in + (static_cast<std::size_t>(2 * y + 1) * size + 2 * x) * 4,
                                                in + (static_cast<std::size_t>(2 * y + 1) * size + 2 * x + 1) * 4};
                    std::uint8_t* o = out + (static_cast<std::size_t>(y) * half + x) * 4;
                    for (int c = 0; c < 4; ++c) {
                        if (srgb && c < 3) {
                            const std::uint32_t sum = tables.toLinear[p[0][c]] + tables.toLinear[p[1][c]]
                                                    + tables.toLinear[p[2][c]] + tables.toLinear[p[3][c]];
                            o[c] = tables.ToSrgb((sum + 2) / 4);
                        } else {
                            o[c] = static_cast<std::uint8_t>((p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) / 4);
                        }
                    }
                }
            }
        }
    }

    std::uint32_t MipLevelCount(std::uint32_t size) {
        return static_cast<std::uint32_t>(std::bit_width(std::max(size, 1u)));
    }

    std::vector<std::uint8_t> BuildMips(std::span<const std::uint8_t> rgba, std::uint32_t size, bool srgb) {
        std::size_t total = 0;
        for (std::uint32_t s = size; s > 0; s /= 2) {
            total += static_cast<std::size_t>(s) * s * 4;
        }
        std::vector<std::uint8_t> levels(total);
        std::memcpy(levels.data(), rgba.data(), static_cast<std::size_t>(size) * size * 4);
        std::size_t at = 0;
        for (std::uint32_t s = size; s > 1; s /= 2) {
            const std::size_t bytes = static_cast<std::size_t>(s) * s * 4;
            Halve(levels.data() + at, s, levels.data() + at + bytes, srgb);
            at += bytes;
        }
        return levels;
    }

    std::size_t BC7Size(std::uint32_t width, std::uint32_t height) {
        return static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4) * 16;
    }

    void CompressBC7(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height, bool perceptual,
                     std::span<std::uint8_t> out) {
        static std::once_flag initialized;
        std::call_once(initialized, [] { bc7enc_compress_block_init(); });
        // Niveau de qualité le plus rapide (uber 0) : une vingtaine d'images de 2048² avec leurs mips à compresser à
        // chaque chargement ; les niveaux plus lents gagnent peu sur des textures de sol (bruit, pas d'aplats).
        bc7enc_compress_block_params params;
        bc7enc_compress_block_params_init(&params);
        if (!perceptual) {
            bc7enc_compress_block_params_init_linear_weights(&params);
        }
        const std::uint32_t blocksX = (width + 3) / 4;
        const std::uint32_t blocksY = (height + 3) / 4;
        std::uint8_t pixels[16 * 4];
        for (std::uint32_t by = 0; by < blocksY; ++by) {
            for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
                // Bords des petits mips (moins de 4 pixels) : le dernier pixel répété.
                for (std::uint32_t y = 0; y < 4; ++y) {
                    const std::uint32_t sy = std::min(by * 4 + y, height - 1);
                    for (std::uint32_t x = 0; x < 4; ++x) {
                        const std::uint32_t sx = std::min(bx * 4 + x, width - 1);
                        std::memcpy(pixels + (y * 4 + x) * 4, rgba.data() + (static_cast<std::size_t>(sy) * width + sx) * 4, 4);
                    }
                }
                bc7enc_compress_block(out.data() + (static_cast<std::size_t>(by) * blocksX + bx) * 16, pixels, &params);
            }
        }
    }
}
