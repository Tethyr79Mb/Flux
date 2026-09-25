#pragma once

#include "Flux/Math/Vector3.hpp"

#include <array>
#include <cmath>

namespace Flux::Math {
    // Matrice 4x4 float, stockée par colonnes (columns[c][r]) ; s'applique à droite : M * v.
    // Toujours en float : les positions double sont ramenées près de la caméra avant d'arriver ici.
    struct Matrix4f
    {
        std::array<std::array<float, 4>, 4> columns{};

        static constexpr Matrix4f Identity() {
            Matrix4f m;
            for (int i = 0; i < 4; ++i) {
                m.columns[i][i] = 1.0f;
            }
            return m;
        }

        static constexpr Matrix4f Translation(const Vector3f& t) {
            Matrix4f m = Identity();
            m.columns[3] = {t.x, t.y, t.z, 1.0f};
            return m;
        }

        static constexpr Matrix4f Scale(const Vector3f& s) {
            Matrix4f m;
            m.columns[0][0] = s.x;
            m.columns[1][1] = s.y;
            m.columns[2][2] = s.z;
            m.columns[3][3] = 1.0f;
            return m;
        }

        // Vue d'une caméra placée à l'origine (rendu relatif à la caméra), à partir de ses axes monde.
        // Repère main droite : x à droite, y en haut, la caméra regarde vers -z.
        static constexpr Matrix4f View(const Vector3f& right, const Vector3f& up, const Vector3f& forward) {
            Matrix4f m;
            m.columns[0] = {right.x, up.x, -forward.x, 0.0f};
            m.columns[1] = {right.y, up.y, -forward.y, 0.0f};
            m.columns[2] = {right.z, up.z, -forward.z, 0.0f};
            m.columns[3] = {0.0f, 0.0f, 0.0f, 1.0f};
            return m;
        }

        // Perspective Vulkan en reverse-Z sans plan lointain : profondeur 1 sur le plan proche, tend vers 0 à l'infini.
        // La précision du float est alors répartie presque uniformément (en log) : indispensable à l'échelle galaxie.
        // Test de profondeur : GREATER, effacement à 0. L'axe y est retourné pour garder y vers le haut à l'écran.
        static Matrix4f PerspectiveReverseZ(float verticalFovRadians, float aspect, float nearPlane) {
            const float f = 1.0f / std::tan(verticalFovRadians * 0.5f);
            Matrix4f m;
            m.columns[0][0] = f / aspect;
            m.columns[1][1] = -f;
            m.columns[2][3] = -1.0f;      // w = -z (distance devant la caméra)
            m.columns[3][2] = nearPlane;  // z = near, donc profondeur = near / distance
            return m;
        }

        constexpr Matrix4f operator*(const Matrix4f& b) const {
            Matrix4f result;
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) {
                    float sum = 0.0f;
                    for (int k = 0; k < 4; ++k) {
                        sum += columns[k][r] * b.columns[c][k];
                    }
                    result.columns[c][r] = sum;
                }
            }
            return result;
        }

        // Transforme un point (w = 1) ; retourne x, y, z, w en coordonnées homogènes.
        constexpr std::array<float, 4> TransformPoint(const Vector3f& p) const {
            std::array<float, 4> out{};
            for (int r = 0; r < 4; ++r) {
                out[r] = columns[0][r] * p.x + columns[1][r] * p.y + columns[2][r] * p.z + columns[3][r];
            }
            return out;
        }
    };
}
