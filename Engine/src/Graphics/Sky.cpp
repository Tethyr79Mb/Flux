#include "Flux/Graphics/Sky.hpp"

#include "Flux/Scene/Components.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <thread>

namespace Flux {
    namespace {
        // Bruit de valeur sur une grille 3D, en entiers 32 bits : le shader (shaders/Sky.slang) fait exactement les
        // mêmes calculs. Toute modification doit être faite des deux côtés.
        std::uint32_t Hash(std::uint32_t x) {
            x ^= x >> 16;
            x *= 0x7feb352dU;
            x ^= x >> 15;
            x *= 0x846ca68bU;
            x ^= x >> 16;
            return x;
        }

        std::uint32_t HashCell(std::int32_t x, std::int32_t y, std::int32_t z, std::uint32_t seed) {
            return Hash(static_cast<std::uint32_t>(x) ^ Hash(static_cast<std::uint32_t>(y) ^ Hash(static_cast<std::uint32_t>(z) ^ seed)));
        }

        float ToUnit(std::uint32_t hash) {
            return static_cast<float>(hash >> 8) * (1.0f / 16777216.0f);
        }

        float Lerp(float a, float b, float t) {
            return a + (b - a) * t;
        }

        float SmoothStep(float edge0, float edge1, float x) {
            const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // Entre 0 et 1, continu, une valeur au hasard à chaque nœud entier.
        float ValueNoise(const Math::Vector3f& p, std::uint32_t seed) {
            const float fx = std::floor(p.x);
            const float fy = std::floor(p.y);
            const float fz = std::floor(p.z);
            const auto x = static_cast<std::int32_t>(fx);
            const auto y = static_cast<std::int32_t>(fy);
            const auto z = static_cast<std::int32_t>(fz);
            const auto smooth = [](float t) { return t * t * (3.0f - 2.0f * t); };
            const float u = smooth(p.x - fx);
            const float v = smooth(p.y - fy);
            const float w = smooth(p.z - fz);
            const auto corner = [&](std::int32_t dx, std::int32_t dy, std::int32_t dz) {
                return ToUnit(HashCell(x + dx, y + dy, z + dz, seed));
            };
            const float x00 = Lerp(corner(0, 0, 0), corner(1, 0, 0), u);
            const float x10 = Lerp(corner(0, 1, 0), corner(1, 1, 0), u);
            const float x01 = Lerp(corner(0, 0, 1), corner(1, 0, 1), u);
            const float x11 = Lerp(corner(0, 1, 1), corner(1, 1, 1), u);
            return Lerp(Lerp(x00, x10, v), Lerp(x01, x11, v), w);
        }

        // Somme d'octaves de plus en plus fines et faibles (« fractal Brownian motion »), ramenée entre 0 et 1.
        float Fbm(Math::Vector3f p, int octaves, std::uint32_t seed) {
            float sum = 0.0f;
            float amplitude = 0.5f;
            float total = 0.0f;
            for (int i = 0; i < octaves; ++i) {
                sum += amplitude * ValueNoise(p, seed + static_cast<std::uint32_t>(i) * 0x9e3779b9U);
                total += amplitude;
                amplitude *= 0.5f;
                p = p * 2.03f + Math::Vector3f{17.1f, 9.7f, 3.3f};
            }
            return sum / total;
        }

        // Vecteur unitaire tiré de la graine.
        Math::Vector3f RandomDirection(std::uint32_t seed) {
            const float z = ToUnit(Hash(seed)) * 2.0f - 1.0f;
            const float angle = ToUnit(Hash(seed ^ 0x68e31da4U)) * 2.0f * std::numbers::pi_v<float>;
            const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
            return {r * std::cos(angle), r * std::sin(angle), z};
        }

        std::uint8_t EncodeSrgb(float linear) {
            const float c = Math::LinearColor::LinearToSrgb(std::clamp(linear, 0.0f, 1.0f));
            return static_cast<std::uint8_t>(c * 255.0f + 0.5f);
        }

        // Les 6 faces remplies en parallèle par fill(direction) -> couleur linéaire, au centre de chaque texel.
        template <typename Fill>
        std::vector<std::uint8_t> BuildCube(std::uint32_t size, const Fill& fill) {
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size) * size * 4 * 6);
            const auto face = [&](int f) {
                std::uint8_t* out = pixels.data() + static_cast<std::size_t>(f) * size * size * 4;
                for (std::uint32_t y = 0; y < size; ++y) {
                    for (std::uint32_t x = 0; x < size; ++x) {
                        const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
                        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
                        const Math::Vector3f color = fill(f, u, v);
                        *out++ = EncodeSrgb(color.x);
                        *out++ = EncodeSrgb(color.y);
                        *out++ = EncodeSrgb(color.z);
                        *out++ = 255;
                    }
                }
            };
            std::array<std::thread, 5> threads;
            for (int f = 1; f < 6; ++f) {
                threads[f - 1] = std::thread(face, f);
            }
            face(0);
            for (std::thread& thread : threads) {
                thread.join();
            }
            return pixels;
        }
    }

    SkyParameters MakeSkyParameters(const SkyComponent& sky) {
        SkyParameters parameters;
        parameters.seed = Hash(static_cast<std::uint32_t>(sky.seed) ^ 0x5bd1e995U);
        parameters.noiseOffset = Math::Vector3f{ToUnit(Hash(parameters.seed + 1)), ToUnit(Hash(parameters.seed + 2)),
                                                ToUnit(Hash(parameters.seed + 3))} * 1000.0f;
        parameters.galaxyAxis = RandomDirection(parameters.seed + 4);
        // Une direction quelconque du plan de la galaxie : la projection d'une autre direction tirée au hasard.
        Math::Vector3f other = RandomDirection(parameters.seed + 5);
        other -= parameters.galaxyAxis * Math::Dot(other, parameters.galaxyAxis);
        parameters.galaxyCenter = Math::Length(other) > 1.0e-3f ? Math::Normalize(other)
                                                                : Math::Normalize(Math::Cross(parameters.galaxyAxis, Math::Vector3f{0.0f, 1.0f, 0.0f}));
        parameters.nebulaColor = sky.nebulaColor;
        parameters.nebulaColor2 = sky.nebulaColor2;
        parameters.nebula = std::clamp(sky.nebula, 0.0f, 1.0f);
        parameters.galaxy = std::clamp(sky.galaxy, 0.0f, 1.0f);
        return parameters;
    }

    Math::Vector3f SkyBackground(const Math::Vector3f& direction, const SkyParameters& sky) {
        const std::uint32_t s = sky.seed;
        const Math::Vector3f p = direction * 2.2f + sky.noiseOffset;
        Math::Vector3f color{0.0f, 0.0f, 0.0f};

        if (sky.nebula > 0.0f) {
            // Nuages déformés par un autre bruit (« domain warping ») : des volutes plutôt que des taches rondes.
            const Math::Vector3f warp{Fbm(p + Math::Vector3f{5.2f, 1.3f, 8.1f}, 4, s + 11),
                                      Fbm(p + Math::Vector3f{1.7f, 9.2f, 3.4f}, 4, s + 12),
                                      Fbm(p + Math::Vector3f{8.3f, 2.8f, 6.1f}, 4, s + 13)};
            const Math::Vector3f q = p * 0.7f + (warp - Math::Vector3f{0.5f, 0.5f, 0.5f}) * 3.0f;
            const float n = Fbm(q, 6, s + 14);
            // Le cœur dense des nuages, et un voile léger autour : pas de bord net.
            const float threshold = Lerp(0.64f, 0.4f, sky.nebula);
            const float core = SmoothStep(threshold, threshold + 0.3f, n);
            const float veil = SmoothStep(threshold - 0.2f, threshold + 0.1f, n);
            // Filaments : un bruit plus fin creuse les nuages.
            const float filaments = 0.55f + 0.45f * SmoothStep(0.35f, 0.65f, Fbm(q * 3.0f + Math::Vector3f{9.1f, 2.6f, 5.3f}, 4, s + 18));
            const float tone = SmoothStep(0.4f, 0.6f, Fbm(q * 0.8f + Math::Vector3f{3.1f, 7.4f, 1.2f}, 3, s + 15));
            const float glow = (core * core * filaments + 0.12f * veil) * (0.4f + 0.6f * sky.nebula);
            color += Math::Vector3f{Lerp(sky.nebulaColor.r, sky.nebulaColor2.r, tone), Lerp(sky.nebulaColor.g, sky.nebulaColor2.g, tone),
                                    Lerp(sky.nebulaColor.b, sky.nebulaColor2.b, tone)} * glow;
        }

        if (sky.galaxy > 0.0f) {
            // Bande laiteuse sur un grand cercle, plus large et plus claire vers le centre, coupée de poussières.
            const float height = Math::Dot(direction, sky.galaxyAxis);
            const float along = Math::Dot(direction, sky.galaxyCenter);
            const float width = 0.1f + 0.08f * (along * 0.5f + 0.5f);
            const float band = std::exp(-(height * height) / (width * width));
            const float core = std::exp(-(1.0f - along) * 4.0f);
            const float haze = Fbm(p * 2.5f + Math::Vector3f{11.0f, 4.0f, 7.0f}, 5, s + 16);
            const float dust = SmoothStep(0.5f, 0.62f, Fbm(p * 4.0f + Math::Vector3f{23.0f, 5.0f, 2.0f}, 5, s + 17))
                             * std::exp(-(height * height) / 0.0025f);
            const float light = band * (0.25f + 0.75f * haze) * (1.0f + 1.5f * core) * (1.0f - 0.85f * dust) * sky.galaxy * 0.3f;
            color += Math::Vector3f{1.0f, 0.88f, 0.72f} * light;
        }
        return color;
    }

    Math::Vector3f SunDirection(float elevationDegrees, float azimuthDegrees) {
        constexpr float RadiansPerDegree = std::numbers::pi_v<float> / 180.0f;
        const float elevation = elevationDegrees * RadiansPerDegree;
        const float azimuth = azimuthDegrees * RadiansPerDegree;
        return {std::cos(elevation) * std::sin(azimuth), std::sin(elevation), std::cos(elevation) * std::cos(azimuth)};
    }

    Math::Vector3f CubeFaceDirection(int face, float u, float v) {
        Math::Vector3f d;
        switch (face) {
            case 0: d = {1.0f, -v, -u}; break;
            case 1: d = {-1.0f, -v, u}; break;
            case 2: d = {u, 1.0f, v}; break;
            case 3: d = {u, -1.0f, -v}; break;
            case 4: d = {u, -v, 1.0f}; break;
            default: d = {-u, -v, -1.0f}; break;
        }
        return Math::Normalize(d);
    }

    void PanoramaUv(const Math::Vector3f& direction, float& u, float& v) {
        constexpr float Pi = std::numbers::pi_v<float>;
        u = std::atan2(direction.x, -direction.z) / (2.0f * Pi) + 0.5f; // le centre de l'image vers -z
        v = std::acos(std::clamp(direction.y, -1.0f, 1.0f)) / Pi;
    }

    std::vector<std::uint8_t> GenerateSkyCube(const SkyParameters& sky, std::uint32_t size) {
        return BuildCube(size, [&](int face, float u, float v) { return SkyBackground(CubeFaceDirection(face, u, v), sky); });
    }

    std::vector<std::uint8_t> PanoramaToCube(const ImageData& panorama, std::uint32_t size) {
        if (panorama.size.x <= 0 || panorama.size.y <= 0) {
            return GradientCube({}, {}, size);
        }
        std::array<float, 256> toLinear{};
        for (int i = 0; i < 256; ++i) {
            toLinear[static_cast<std::size_t>(i)] = Math::LinearColor::SrgbToLinear(static_cast<float>(i) / 255.0f);
        }
        const int width = panorama.size.x;
        const int height = panorama.size.y;
        // Lecture bilinéaire (en linéaire), qui fait le tour en largeur.
        const auto sample = [&](float u, float v) {
            const float x = u * static_cast<float>(width) - 0.5f;
            const float y = std::clamp(v * static_cast<float>(height) - 0.5f, 0.0f, static_cast<float>(height - 1));
            const float fx = std::floor(x);
            const float fy = std::floor(y);
            const float tx = x - fx;
            const float ty = y - fy;
            const int x0 = ((static_cast<int>(fx) % width) + width) % width;
            const int x1 = (x0 + 1) % width;
            const int y0 = static_cast<int>(fy);
            const int y1 = std::min(y0 + 1, height - 1);
            Math::Vector3f result{0.0f, 0.0f, 0.0f};
            const auto add = [&](int px, int py, float weight) {
                const std::uint8_t* texel = panorama.rgba.data() + (static_cast<std::size_t>(py) * width + px) * 4;
                result += Math::Vector3f{toLinear[texel[0]], toLinear[texel[1]], toLinear[texel[2]]} * weight;
            };
            add(x0, y0, (1.0f - tx) * (1.0f - ty));
            add(x1, y0, tx * (1.0f - ty));
            add(x0, y1, (1.0f - tx) * ty);
            add(x1, y1, tx * ty);
            return result;
        };
        // 4 x 4 points par texel : un détail fin de l'image (une étoile) compte pour sa part au lieu d'apparaître ou non.
        constexpr int Samples = 4;
        const float step = 2.0f / static_cast<float>(size);
        return BuildCube(size, [&](int face, float u, float v) {
            Math::Vector3f sum{0.0f, 0.0f, 0.0f};
            for (int sy = 0; sy < Samples; ++sy) {
                for (int sx = 0; sx < Samples; ++sx) {
                    const float du = ((static_cast<float>(sx) + 0.5f) / Samples - 0.5f) * step;
                    const float dv = ((static_cast<float>(sy) + 0.5f) / Samples - 0.5f) * step;
                    float pu = 0.0f;
                    float pv = 0.0f;
                    PanoramaUv(CubeFaceDirection(face, u + du, v + dv), pu, pv);
                    sum += sample(pu, pv);
                }
            }
            return sum * (1.0f / (Samples * Samples));
        });
    }

    std::vector<std::uint8_t> GradientCube(const Math::LinearColor& top, const Math::LinearColor& bottom, std::uint32_t size) {
        return BuildCube(size, [&](int face, float u, float v) {
            const float t = CubeFaceDirection(face, u, v).y * 0.5f + 0.5f;
            return Math::Vector3f{Lerp(bottom.r, top.r, t), Lerp(bottom.g, top.g, t), Lerp(bottom.b, top.b, t)};
        });
    }
}
