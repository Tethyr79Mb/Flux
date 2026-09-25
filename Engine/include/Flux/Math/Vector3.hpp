#pragma once

#include <cmath>

namespace Flux::Math {
    template <typename T>
    struct Vector3
    {
        T x{};
        T y{};
        T z{};

        constexpr Vector3() = default;
        constexpr Vector3(T _x, T _y, T _z) : x(_x), y(_y), z(_z) {}

        // Conversion explicite entre précisions (double -> float après passage en coordonnées relatives à la caméra).
        template <typename U>
        constexpr explicit Vector3(const Vector3<U>& other)
            : x(static_cast<T>(other.x)), y(static_cast<T>(other.y)), z(static_cast<T>(other.z)) {}

        constexpr bool operator==(const Vector3&) const = default;

        constexpr Vector3 operator-() const { return {-x, -y, -z}; }
        constexpr Vector3 operator+(const Vector3& v) const { return {x + v.x, y + v.y, z + v.z}; }
        constexpr Vector3 operator-(const Vector3& v) const { return {x - v.x, y - v.y, z - v.z}; }
        constexpr Vector3 operator*(T s) const { return {x * s, y * s, z * s}; }
        constexpr Vector3 operator/(T s) const { return {x / s, y / s, z / s}; }
        constexpr Vector3& operator+=(const Vector3& v) { x += v.x; y += v.y; z += v.z; return *this; }
        constexpr Vector3& operator-=(const Vector3& v) { x -= v.x; y -= v.y; z -= v.z; return *this; }
        constexpr Vector3& operator*=(T s) { x *= s; y *= s; z *= s; return *this; }
    };

    template <typename T>
    constexpr Vector3<T> operator*(T s, const Vector3<T>& v) { return v * s; }

    template <typename T>
    constexpr T Dot(const Vector3<T>& a, const Vector3<T>& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    template <typename T>
    constexpr Vector3<T> Cross(const Vector3<T>& a, const Vector3<T>& b) {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

    template <typename T>
    T Length(const Vector3<T>& v) { return std::sqrt(Dot(v, v)); }

    // Retourne le vecteur nul si v est nul.
    template <typename T>
    Vector3<T> Normalize(const Vector3<T>& v) {
        const T length = Length(v);
        return length > T(0) ? v / length : Vector3<T>{};
    }

    using Vector3f = Vector3<float>;
    using Vector3d = Vector3<double>;
}
