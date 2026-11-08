#pragma once

#include <cmath>

namespace Flux::Math {
    // Couleur en espace linéaire, composantes entre 0 et 1.
    struct LinearColor
    {
        float r{};
        float g{};
        float b{};
        float a{1.0f};

        constexpr bool operator==(const LinearColor&) const = default;

        // Conversions d'une composante entre sRGB (ce qu'affiche un sélecteur de couleur) et linéaire, entre 0 et 1.
        static float SrgbToLinear(float c) {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        static float LinearToSrgb(float c) {
            return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        }

        // Depuis une couleur sRGB 8 bits (celle d'un sélecteur de couleur ou d'une maquette) ; alpha déjà linéaire.
        static LinearColor FromSrgb8(int red, int green, int blue, float alpha = 1.0f) {
            const auto decode = [](int value) { return SrgbToLinear(static_cast<float>(value) / 255.0f); };
            return {decode(red), decode(green), decode(blue), alpha};
        }
    };
}
