#pragma once

#include "Flux/Math/Vector3.hpp"

namespace Flux::Math {
    // Boîte alignée sur les axes (dans le repère local d'un maillage). Par défaut : le cube unité centré.
    struct Box3f
    {
        Vector3f min{-0.5f, -0.5f, -0.5f};
        Vector3f max{0.5f, 0.5f, 0.5f};

        constexpr bool operator==(const Box3f&) const = default;
        constexpr Vector3f GetCenter() const { return (min + max) * 0.5f; }
        constexpr Vector3f GetSize() const { return max - min; }
    };
}
