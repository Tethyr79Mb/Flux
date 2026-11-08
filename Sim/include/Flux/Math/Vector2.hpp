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

        constexpr Vector2 operator+(const Vector2& v) const { return {x + v.x, y + v.y}; }
        constexpr Vector2 operator-(const Vector2& v) const { return {x - v.x, y - v.y}; }
        constexpr Vector2 operator*(T s) const { return {x * s, y * s}; }
        constexpr Vector2& operator+=(const Vector2& v) { x += v.x; y += v.y; return *this; }
        constexpr Vector2& operator-=(const Vector2& v) { x -= v.x; y -= v.y; return *this; }
    };

    using Vector2f = Vector2<float>;
    using Vector2i = Vector2<int>;
}
