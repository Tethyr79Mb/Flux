#include "Flux/Assets/Image.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>

namespace Flux {
    namespace {
        float SrgbToLinear(float c) {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        // Tables : 3 conversions par pixel, des millions de pixels pour une 4096.
        const std::array<float, 256>& DecodeTable() {
            static const std::array<float, 256> table = [] {
                std::array<float, 256> t{};
                for (std::size_t i = 0; i < t.size(); ++i) {
                    t[i] = SrgbToLinear(static_cast<float>(i) / 255.0f);
                }
                return t;
            }();
            return table;
        }

        constexpr std::size_t EncodeSize = 4096;
        const std::array<std::uint8_t, EncodeSize>& EncodeTable() {
            static const std::array<std::uint8_t, EncodeSize> table = [] {
                std::array<std::uint8_t, EncodeSize> t{};
                for (std::size_t i = 0; i < t.size(); ++i) {
                    const float c = static_cast<float>(i) / static_cast<float>(EncodeSize - 1);
                    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
                    t[i] = static_cast<std::uint8_t>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
                }
                return t;
            }();
            return table;
        }

        std::uint8_t ToByte(float v) {
            return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        }

        struct Level
        {
            int width = 0;
            int height = 0;
            std::vector<float> pixels; // linéaire, alpha non prémultiplié, 4 par pixel
        };

        // Part des pixels dont l'alpha, multiplié par scale, atteint le seuil.
        float Coverage(const Level& level, float cutoff, float scale) {
            std::size_t passed = 0;
            const std::size_t count = level.pixels.size() / 4;
            for (std::size_t i = 0; i < count; ++i) {
                passed += level.pixels[i * 4 + 3] * scale >= cutoff ? 1 : 0;
            }
            return count > 0 ? static_cast<float>(passed) / static_cast<float>(count) : 0.0f;
        }

        // Échelle de l'alpha qui donne la couverture voulue : seuil divisé par l'alpha du pixel de rang
        // couverture × nombre, du plus opaque au plus transparent.
        float CoverageScale(const Level& level, float cutoff, float coverage) {
            const std::size_t count = level.pixels.size() / 4;
            const auto wanted = static_cast<std::size_t>(std::lround(coverage * static_cast<float>(count)));
            if (wanted == 0 || wanted >= count) {
                return 1.0f; // rien ou tout passe : rien à garder
            }
            std::vector<float> alphas(count);
            for (std::size_t i = 0; i < count; ++i) {
                alphas[i] = level.pixels[i * 4 + 3];
            }
            std::nth_element(alphas.begin(), alphas.begin() + static_cast<std::ptrdiff_t>(wanted - 1), alphas.end(), std::greater<>());
            const float threshold = alphas[wanted - 1];
            // Un pixel presque transparent ne doit pas devenir opaque : échelle bornée (un texel sur 4 d'une branche
            // d'un pixel suffit déjà à quadrupler).
            return threshold > 0.0f ? std::clamp(cutoff / threshold, 0.25f, 16.0f) : 16.0f;
        }

        Level Reduce(const Level& source) {
            Level level;
            level.width = std::max(source.width / 2, 1);
            level.height = std::max(source.height / 2, 1);
            level.pixels.resize(static_cast<std::size_t>(level.width) * level.height * 4);
            for (int y = 0; y < level.height; ++y) {
                for (int x = 0; x < level.width; ++x) {
                    float color[3] = {};
                    float plainColor[3] = {};
                    float alpha = 0.0f;
                    int samples = 0;
                    // 2 x 2 pixels (1 sur un côté de 1) ; un côté impair perd sa dernière rangée, comme le blit du GPU.
                    for (int dy = 0; dy < (source.height > 1 ? 2 : 1); ++dy) {
                        for (int dx = 0; dx < (source.width > 1 ? 2 : 1); ++dx) {
                            const int sx = std::min(x * 2 + dx, source.width - 1);
                            const int sy = std::min(y * 2 + dy, source.height - 1);
                            const float* p = &source.pixels[(static_cast<std::size_t>(sy) * source.width + sx) * 4];
                            for (int c = 0; c < 3; ++c) {
                                color[c] += p[c] * p[3];
                                plainColor[c] += p[c];
                            }
                            alpha += p[3];
                            ++samples;
                        }
                    }
                    float* out = &level.pixels[(static_cast<std::size_t>(y) * level.width + x) * 4];
                    for (int c = 0; c < 3; ++c) {
                        out[c] = alpha > 0.0f ? color[c] / alpha : plainColor[c] / static_cast<float>(samples);
                    }
                    out[3] = alpha / static_cast<float>(samples);
                }
            }
            return level;
        }

        void Append(std::vector<std::uint8_t>& out, const Level& level, float alphaScale) {
            const auto& encode = EncodeTable();
            const std::size_t count = level.pixels.size() / 4;
            for (std::size_t i = 0; i < count; ++i) {
                const float* p = &level.pixels[i * 4];
                for (int c = 0; c < 3; ++c) {
                    out.push_back(encode[static_cast<std::size_t>(std::clamp(p[c], 0.0f, 1.0f) * (EncodeSize - 1) + 0.5f)]);
                }
                out.push_back(ToByte(p[3] * alphaScale));
            }
        }
    }

    std::vector<std::uint8_t> BuildMipChain(const ImageData& image, float alphaCutoff, std::uint32_t& levels) {
        levels = 0;
        const std::size_t pixelCount = static_cast<std::size_t>(std::max(image.size.x, 0)) * std::max(image.size.y, 0);
        if (pixelCount == 0 || image.rgba.size() != pixelCount * 4) {
            return {};
        }
        levels = static_cast<std::uint32_t>(std::bit_width(static_cast<std::uint32_t>(std::max(image.size.x, image.size.y))));
        std::vector<std::uint8_t> out;
        out.reserve(pixelCount * 4 * 4 / 3 + 64);
        out.insert(out.end(), image.rgba.begin(), image.rgba.end()); // niveau 0 tel quel

        const auto& decode = DecodeTable();
        Level level;
        level.width = image.size.x;
        level.height = image.size.y;
        level.pixels.resize(pixelCount * 4);
        for (std::size_t i = 0; i < pixelCount; ++i) {
            for (int c = 0; c < 3; ++c) {
                level.pixels[i * 4 + c] = decode[image.rgba[i * 4 + c]];
            }
            level.pixels[i * 4 + 3] = static_cast<float>(image.rgba[i * 4 + 3]) / 255.0f;
        }
        const float coverage = alphaCutoff > 0.0f ? Coverage(level, alphaCutoff, 1.0f) : 0.0f;
        for (std::uint32_t l = 1; l < levels; ++l) {
            // Chaque niveau réduit du précédent tel quel (pas mis à l'échelle) : les erreurs ne s'additionnent pas.
            level = Reduce(level);
            Append(out, level, alphaCutoff > 0.0f ? CoverageScale(level, alphaCutoff, coverage) : 1.0f);
        }
        return out;
    }
}
