#pragma once

#include "Flux/Math/Matrix4.hpp"
#include "Flux/Math/Vector3.hpp"

#include <cmath>

namespace Flux::Math {
    // Rotation unitaire (x, y, z vectoriels, w scalaire). Les angles sont en radians.
    struct Quaternionf
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 1.0f;

        static constexpr Quaternionf Identity() { return {}; }

        static Quaternionf FromAxisAngle(const Vector3f& axis, float angle) {
            const Vector3f n = Normalize(axis);
            const float s = std::sin(angle * 0.5f);
            return {n.x * s, n.y * s, n.z * s, std::cos(angle * 0.5f)};
        }

        // Lacet autour de y, puis tangage autour de x, puis roulis autour de z (comme la caméra).
        static Quaternionf FromYawPitchRoll(float yaw, float pitch, float roll) {
            return FromAxisAngle({0, 1, 0}, yaw) * FromAxisAngle({1, 0, 0}, pitch) * FromAxisAngle({0, 0, 1}, roll);
        }

        constexpr bool operator==(const Quaternionf&) const = default;

        // Rotation inverse (pour un quaternion unitaire).
        constexpr Quaternionf Conjugate() const { return {-x, -y, -z, w}; }

        // Composition : (a * b) applique b puis a.
        constexpr Quaternionf operator*(const Quaternionf& b) const {
            return {
                w * b.x + x * b.w + y * b.z - z * b.y,
                w * b.y - x * b.z + y * b.w + z * b.x,
                w * b.z + x * b.y - y * b.x + z * b.w,
                w * b.w - x * b.x - y * b.y - z * b.z,
            };
        }

        // Tourne un vecteur ; calculé dans la précision du vecteur (double pour les positions).
        template <typename T>
        constexpr Vector3<T> Rotate(const Vector3<T>& v) const {
            const Vector3<T> q{static_cast<T>(x), static_cast<T>(y), static_cast<T>(z)};
            const Vector3<T> t = Cross(q, v) * T(2);
            return v + t * static_cast<T>(w) + Cross(q, t);
        }

        Matrix4f ToMatrix() const {
            const Vector3f right = Rotate(Vector3f{1, 0, 0});
            const Vector3f up = Rotate(Vector3f{0, 1, 0});
            const Vector3f back = Rotate(Vector3f{0, 0, 1});
            Matrix4f m = Matrix4f::Identity();
            m.columns[0] = {right.x, right.y, right.z, 0.0f};
            m.columns[1] = {up.x, up.y, up.z, 0.0f};
            m.columns[2] = {back.x, back.y, back.z, 0.0f};
            return m;
        }
    };

    // Inverse de FromYawPitchRoll : angles (lacet, tangage, roulis) en radians, tangage dans [-pi/2, pi/2].
    // À la verticale (tangage de ±90°), lacet et roulis se confondent : le roulis est alors mis à 0.
    inline Vector3f ToYawPitchRoll(const Quaternionf& q) {
        // Colonnes de la matrice de rotation : R = Ry(lacet) * Rx(tangage) * Rz(roulis).
        const Vector3f column0 = q.Rotate(Vector3f{1, 0, 0});
        const Vector3f column1 = q.Rotate(Vector3f{0, 1, 0});
        const Vector3f column2 = q.Rotate(Vector3f{0, 0, 1});
        const float sinPitch = -column2.y; // R[1][2] = -sin(tangage)
        if (std::abs(sinPitch) > 0.99999f) {
            const float pitch = std::copysign(1.5707963f, sinPitch);
            return {std::atan2(-column0.z, column0.x), pitch, 0.0f};
        }
        return {std::atan2(column2.x, column2.z), std::asin(sinPitch), std::atan2(column0.y, column1.y)};
    }

    inline Quaternionf Normalize(const Quaternionf& q) {
        const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return length > 0.0f ? Quaternionf{q.x / length, q.y / length, q.z / length, q.w / length} : Quaternionf{};
    }
}
