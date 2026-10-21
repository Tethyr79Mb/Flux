#include "Flux/Graphics/ShadowCascades.hpp"

#include "Flux/Graphics/Camera.hpp"

#include <algorithm>
#include <cmath>

namespace Flux {
    std::array<ShadowCascade, ShadowCascadeCount> ComputeShadowCascades(const Camera& camera, float aspect,
                                                                        const Math::Vector3f& toSun, std::uint32_t resolution,
                                                                        float casterDistance) {
        // Repère de la lumière : `forward` suit les rayons (du soleil vers la scène), right et up couvrent la carte.
        const Math::Vector3f forward = Math::Normalize(toSun) * -1.0f;
        const Math::Vector3f reference = std::abs(forward.y) < 0.99f ? Math::Vector3f{0.0f, 1.0f, 0.0f} : Math::Vector3f{1.0f, 0.0f, 0.0f};
        const Math::Vector3f right = Math::Normalize(Math::Cross(forward, reference));
        const Math::Vector3f up = Math::Cross(right, forward);

        const Math::Vector3f viewForward = camera.GetForward();
        const float tanY = std::tan(camera.verticalFov * 0.5f);
        const float tanX = tanY * aspect;
        // Position de la caméra dans son secteur : sert à caler les cascades sur la grille du monde. Un changement de
        // secteur déplace la grille une fois, sans conséquence visible.
        const Math::Vector3d eye = camera.position.offset;
        const double eyeRight = eye.x * right.x + eye.y * right.y + eye.z * right.z;
        const double eyeUp = eye.x * up.x + eye.y * up.y + eye.z * up.z;

        std::array<ShadowCascade, ShadowCascadeCount> cascades{};
        float start = camera.nearPlane;
        for (int i = 0; i < ShadowCascadeCount; ++i) {
            const float end = ShadowCascadeEnds[static_cast<std::size_t>(i)];
            // Sphère de la tranche [start, end] : centre sur l'axe de la vue, là où elle touche à la fois les coins
            // proches et lointains (ou au bout, si la tranche est très évasée).
            const float cornerFactor = tanX * tanX + tanY * tanY; // distance latérale² d'un coin / profondeur²
            float centerDistance = 0.5f * (start + end) * (1.0f + cornerFactor);
            centerDistance = std::min(centerDistance, end);
            const auto cornerDistance = [&](float depth) {
                const float along = depth - centerDistance;
                return std::sqrt(along * along + depth * depth * cornerFactor);
            };
            float radius = std::max(cornerDistance(start), cornerDistance(end));
            // Taille arrondie au décimètre au-dessus : elle ne change pas d'une image à l'autre (redimensionnement).
            radius = std::ceil(radius * 10.0f) / 10.0f;
            const float texel = 2.0f * radius / static_cast<float>(resolution);

            // Centre calé sur la grille des texels, dans le repère du monde (pas de la caméra, qui bouge).
            Math::Vector3f center = viewForward * centerDistance;
            const double worldRight = eyeRight + static_cast<double>(Math::Dot(center, right));
            const double worldUp = eyeUp + static_cast<double>(Math::Dot(center, up));
            const double snappedRight = std::floor(worldRight / texel) * texel;
            const double snappedUp = std::floor(worldUp / texel) * texel;
            center += right * static_cast<float>(snappedRight - worldRight) + up * static_cast<float>(snappedUp - worldUp);

            // x = (p - c)·right / r ; y = (p - c)·up / r ; profondeur de casterDistance devant la tranche à r derrière.
            const float depthRange = casterDistance + radius;
            Math::Matrix4f m;
            m.columns[0] = {right.x / radius, up.x / radius, forward.x / depthRange, 0.0f};
            m.columns[1] = {right.y / radius, up.y / radius, forward.y / depthRange, 0.0f};
            m.columns[2] = {right.z / radius, up.z / radius, forward.z / depthRange, 0.0f};
            m.columns[3] = {-Math::Dot(center, right) / radius, -Math::Dot(center, up) / radius,
                            (casterDistance - Math::Dot(center, forward)) / depthRange, 1.0f};
            cascades[static_cast<std::size_t>(i)] = {m, center, radius, texel, depthRange};
            start = end;
        }
        return cascades;
    }

    ShadowCascade MoveShadowCascade(const ShadowCascade& cascade, const Math::Vector3f& cameraOffset) {
        // Un point fixe du monde est décalé de -cameraOffset par rapport à la nouvelle caméra : la translation compense.
        ShadowCascade moved = cascade;
        moved.center = cascade.center - cameraOffset;
        auto& c = moved.lightViewProjection.columns;
        for (int row = 0; row < 3; ++row) {
            c[3][row] += c[0][row] * cameraOffset.x + c[1][row] * cameraOffset.y + c[2][row] * cameraOffset.z;
        }
        return moved;
    }
}
