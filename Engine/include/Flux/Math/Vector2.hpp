#pragma once

namespace Flux::Math {
    template <typename T>
    struct Vector2
    {
        T x{};
        T y{};

        constexpr Vector2() = default;
        constexpr Vector2(T _x, T _y) : x(_x), y(_y) {}

        constexpr bool operator==(const Vector2&) const = default;
    };

    using Vector2f = Vector2<float>;
    using Vector2i = Vector2<int>;
}
