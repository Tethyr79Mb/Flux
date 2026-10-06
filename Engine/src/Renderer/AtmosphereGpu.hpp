#pragma once

#include "Flux/Graphics/Atmosphere.hpp"

#include <array>

namespace Flux {
    // Atmosphère de la planète la plus proche, telle que la lisent les shaders (AtmosphereData dans
    // shaders/Atmosphere.slang, mêmes calculs que Flux/Graphics/Atmosphere.hpp). mie[3] (le haut) à 0 : aucune.
    struct AtmosphereGpu
    {
        std::array<float, 4> center{};   // xyz : centre de la planète relatif à la caméra ; w : rayon du sol
        std::array<float, 4> rayleigh{}; // rgb : diffusion au sol ; w : 1 / hauteur d'échelle
        std::array<float, 4> mie{};      // x : diffusion au sol ; y : 1 / hauteur d'échelle ; z : g ; w : rayon du haut
        std::array<float, 4> scale{};    // x : intensité de la lumière diffusée
        std::array<std::array<float, 4>, AtmosphereAmbientSize> ambient{}; // rgb (ComputeAtmosphereAmbient)
    };
    static_assert(sizeof(AtmosphereGpu) == 4 * 16 + AtmosphereAmbientSize * 16);
}
