#include "Renderer/ToneCurve.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace Flux::ToneCurve {
    namespace {
        using Vector = std::array<float, 3>;
        using Matrix = std::array<Vector, 3>; // lignes

        // Doivent correspondre à shaders/Post.slang. Entrée : les primaires resserrées vers le blanc, ce qui fait
        // blanchir les couleurs fortes ; sortie : son inverse.
        constexpr Matrix Inset{{{0.842479062253094f, 0.0784335999999992f, 0.0792237451477643f},
                                {0.0423282422610123f, 0.878468636469772f, 0.0791661274605434f},
                                {0.0423756549057051f, 0.0784336f, 0.879142973793104f}}};
        constexpr Matrix Outset{{{1.19687900512017f, -0.0980208811401368f, -0.0990297440797205f},
                                 {-0.0528968517574562f, 1.15190312990417f, -0.0989611768448433f},
                                 {-0.0529716355144438f, -0.0980434501171241f, 1.15107367264116f}}};
        // Plage de la courbe, en stops autour du gris moyen : 12,5 stops dessous, 4 au-dessus.
        constexpr float MinEv = -12.47393f;
        constexpr float MaxEv = 4.026069f;
        // L'écran : gamma 2,2, que l'encodage sRGB de la fenêtre défait.
        constexpr float DisplayGamma = 2.2f;

        Vector Multiply(const Matrix& m, const Vector& v) {
            return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
                    m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
        }

        // Réglage « Punchy » (comme dans Blender) : plus de contraste et de saturation qu'AgX de base, qui délave.
        constexpr float LookPower = 1.35f;
        constexpr float LookSaturation = 1.4f;
        constexpr Vector Luma{0.2126f, 0.7152f, 0.0722f};

        float Dot(const Vector& a, const Vector& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

        Vector Look(Vector v) {
            for (float& c : v) {
                c = std::pow(std::max(c, 0.0f), LookPower);
            }
            const float luma = Dot(v, Luma);
            for (float& c : v) {
                c = luma + LookSaturation * (c - luma);
            }
            return v;
        }

        // La saturation garde la luminance : elle se défait d'abord, puis la puissance.
        Vector InverseLook(Vector v) {
            const float luma = Dot(v, Luma);
            for (float& c : v) {
                c = std::pow(std::max(luma + (c - luma) / LookSaturation, 0.0f), 1.0f / LookPower);
            }
            return v;
        }

        // Le contraste d'AgX en polynôme de degré 6, croissant de 0 à 1.
        float Contrast(float x) {
            const float x2 = x * x;
            const float x4 = x2 * x2;
            return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
        }

        // x de 0 à 1 tel que Contrast(x) = y, par dichotomie (la courbe croît) : 24 pas, à 1e-7 près.
        float InverseContrast(float y) {
            float low = 0.0f;
            float high = 1.0f;
            for (int i = 0; i < 24; ++i) {
                const float middle = 0.5f * (low + high);
                (Contrast(middle) < y ? low : high) = middle;
            }
            return 0.5f * (low + high);
        }
    }

    Math::LinearColor ToneMap(Math::LinearColor color, float exposure) {
        const Vector inset = Multiply(Inset, {color.r * exposure, color.g * exposure, color.b * exposure});
        Vector curved;
        for (std::size_t i = 0; i < 3; ++i) {
            const float ev = std::clamp(std::log2(std::max(inset[i], 1.0e-10f)), MinEv, MaxEv);
            curved[i] = Contrast((ev - MinEv) / (MaxEv - MinEv));
        }
        const Vector display = Multiply(Outset, Look(curved));
        const auto linear = [](float c) { return std::pow(std::clamp(c, 0.0f, 1.0f), DisplayGamma); };
        return {linear(display[0]), linear(display[1]), linear(display[2]), color.a};
    }

    Math::LinearColor InverseToneMap(Math::LinearColor color, float exposure) {
        const auto encoded = [](float c) { return std::pow(std::clamp(c, 0.0f, 1.0f), 1.0f / DisplayGamma); };
        const Vector curved = InverseLook(Multiply(Inset, {encoded(color.r), encoded(color.g), encoded(color.b)}));
        Vector inset;
        for (std::size_t i = 0; i < 3; ++i) {
            inset[i] = std::exp2(MinEv + InverseContrast(curved[i]) * (MaxEv - MinEv));
        }
        const Vector hdr = Multiply(Outset, inset);
        const float scale = 1.0f / std::max(exposure, 1.0e-4f);
        return {std::max(hdr[0], 0.0f) * scale, std::max(hdr[1], 0.0f) * scale, std::max(hdr[2], 0.0f) * scale, color.a};
    }
}
