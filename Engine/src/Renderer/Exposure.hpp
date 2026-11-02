#pragma once

#include "Flux/Export.hpp"

#include <array>
#include <cstdint>
#include <optional>

// Exposition automatique, côté CPU : de l'histogramme mesuré par le GPU (shaders/Exposure.slang, ExposureRenderer) à
// l'exposition de l'image, comme l'œil qui s'habitue à la lumière. Fonctions pures, testées sans GPU (ExposureTests).
namespace Flux::Exposure {
    // Doivent correspondre à shaders/Exposure.slang : case 0, ce qui est plus sombre que 2^MinLog2 (le noir de
    // l'espace, ignoré) ; cases 1 à Bins - 1, de 2^MinLog2 à 2^MaxLog2 en log2 (0,16 stop chacune).
    inline constexpr std::uint32_t Bins = 128;
    inline constexpr float MinLog2 = -10.0f;
    inline constexpr float MaxLog2 = 10.0f;
    using Histogram = std::array<std::uint32_t, Bins>;

    // Réglages de la scène (composant Ciel), en stops (puissances de 2 de l'exposition).
    struct Settings
    {
        bool automatic = false; // false : exposition fixe, 2^compensation
        float compensation = 0.0f;
        float minimum = -4.0f; // l'image ne s'assombrit pas plus (face au soleil)
        float maximum = 1.0f;  // ni ne s'éclaircit plus (cockpit dans le noir, espace)
        // Vitesse d'adaptation vers une scène plus claire, par seconde (3 : 95 % du chemin en une seconde) ; vers une
        // scène plus sombre, trois fois moins vite, comme l'œil.
        float speed = 3.0f;
    };

    // Luminance moyenne de l'image : moyenne en log2 (géométrique) de la tranche de 40 % à 90 % du poids, des cases
    // les plus sombres aux plus claires. Les ombres profondes et les reflets du soleil n'y comptent pas. Rien si toute
    // l'image est plus sombre que 2^MinLog2.
    FLUX_API std::optional<float> MeteredLuminance(const Histogram& histogram);
    // Exposition qui ramène cette luminance au gris moyen (18 %), décalée de la compensation, dans les bornes.
    FLUX_API float Target(float luminance, const Settings& settings);
    // `current` rapprochée de `target` après `seconds` secondes (en log2 : chaque stop prend le même temps).
    FLUX_API float Adapt(float current, float target, float seconds, const Settings& settings);
}
