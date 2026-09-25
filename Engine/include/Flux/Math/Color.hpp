#pragma once

namespace Flux::Math {
    // Couleur en espace linéaire, composantes entre 0 et 1.
    struct LinearColor
    {
        float r{};
        float g{};
        float b{};
        float a{1.0f};

        constexpr bool operator==(const LinearColor&) const = default;
    };
}
