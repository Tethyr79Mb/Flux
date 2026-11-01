#include "Flux/Graphics/ShadowCascades.hpp"

#include "Flux/Graphics/Camera.hpp"

#include <algorithm>
#include <cmath>

namespace Flux {
    namespace {
        // Repère de la lumière et position de la caméra dans ce repère, communs à toutes les tranches.
        struct LightFrame
        {
            Math::Vector3f forward; // suit les rayons (du soleil vers la scène) ; right et up couvrent la carte
            Math::Vector3f right;
            Math::Vector3f up;
            Math::Vector3f viewForward;
            float cornerFactor; // distance latérale² d'un coin du champ / profondeur²
            double eyeRight;
            double eyeUp;
        };

        LightFrame MakeLightFrame(const Camera& camera, float aspect, const Math::Vector3f& toSun) {
            LightFrame frame{};
            frame.forward = Math::Normalize(toSun) * -1.0f;
            const Math::Vector3f reference = std::abs(frame.forward.y) < 0.99f ? Math::Vector3f{0.0f, 1.0f, 0.0f} : Math::Vector3f{1.0f, 0.0f, 0.0f};
            frame.right = Math::Normalize(Math::Cross(frame.forward, reference));
            frame.up = Math::Cross(frame.right, frame.forward);
            frame.viewForward = camera.GetForward();
            const float tanY = std::tan(camera.verticalFov * 0.5f);
            const float tanX = tanY * aspect;
            frame.cornerFactor = tanX * tanX + tanY * tanY;
            // Position de la caméra dans son secteur : sert à caler les cascades sur la grille du monde. Un changement
            // de secteur déplace la grille une fois, sans conséquence visible.
            const Math::Vector3d eye = camera.position.offset;
            frame.eyeRight = eye.x * frame.right.x + eye.y * frame.right.y + eye.z * frame.right.z;
            frame.eyeUp = eye.x * frame.up.x + eye.y * frame.up.y + eye.z * frame.up.z;
            return frame;
        }

        ShadowCascade CascadeAround(const LightFrame& frame, float centerDistance, float radius, std::uint32_t resolution,
                                    float casterDistance);

        ShadowCascade ComputeCascade(const LightFrame& frame, float start, float end, std::uint32_t resolution,
                                     float casterDistance) {
            // Sphère de la tranche [start, end] : centre sur l'axe de la vue, là où elle touche à la fois les coins
            // proches et lointains (ou au bout, si la tranche est très évasée).
            float centerDistance = 0.5f * (start + end) * (1.0f + frame.cornerFactor);
            centerDistance = std::min(centerDistance, end);
            const auto cornerDistance = [&](float depth) {
                const float along = depth - centerDistance;
                return std::sqrt(along * along + depth * depth * frame.cornerFactor);
            };
            return CascadeAround(frame, centerDistance, std::max(cornerDistance(start), cornerDistance(end)), resolution,
                                 casterDistance);
        }

        // Carte qui couvre la sphère de rayon `radius` à `centerDistance` mètres devant la caméra.
        ShadowCascade CascadeAround(const LightFrame& frame, float centerDistance, float radius, std::uint32_t resolution,
                                    float casterDistance) {
            // Taille arrondie au décimètre au-dessus : elle ne change pas d'une image à l'autre (redimensionnement).
            radius = std::ceil(radius * 10.0f) / 10.0f;
            const float texel = 2.0f * radius / static_cast<float>(resolution);

            // Centre calé sur la grille des texels, dans le repère du monde (pas de la caméra, qui bouge).
            Math::Vector3f center = frame.viewForward * centerDistance;
            const double worldRight = frame.eyeRight + static_cast<double>(Math::Dot(center, frame.right));
            const double worldUp = frame.eyeUp + static_cast<double>(Math::Dot(center, frame.up));
            const double snappedRight = std::floor(worldRight / texel) * texel;
            const double snappedUp = std::floor(worldUp / texel) * texel;
            center += frame.right * static_cast<float>(snappedRight - worldRight) + frame.up * static_cast<float>(snappedUp - worldUp);

            // x = (p - c)·right / r ; y = (p - c)·up / r ; profondeur de casterDistance devant la tranche à r derrière.
            const Math::Vector3f& right = frame.right;
            const Math::Vector3f& up = frame.up;
            const Math::Vector3f& forward = frame.forward;
            const float depthRange = casterDistance + radius;
            Math::Matrix4f m;
            m.columns[0] = {right.x / radius, up.x / radius, forward.x / depthRange, 0.0f};
            m.columns[1] = {right.y / radius, up.y / radius, forward.y / depthRange, 0.0f};
            m.columns[2] = {right.z / radius, up.z / radius, forward.z / depthRange, 0.0f};
            m.columns[3] = {-Math::Dot(center, right) / radius, -Math::Dot(center, up) / radius,
                            (casterDistance - Math::Dot(center, forward)) / depthRange, 1.0f};
            return {m, center, radius, texel, depthRange};
        }
    }

    std::array<ShadowCascade, ShadowCascadeCount> ComputeShadowCascades(const Camera& camera, float aspect,
                                                                        const Math::Vector3f& toSun, std::uint32_t resolution,
                                                                        float casterDistance) {
        const LightFrame frame = MakeLightFrame(camera, aspect, toSun);
        std::array<ShadowCascade, ShadowCascadeCount> cascades{};
        float start = camera.nearPlane;
        for (std::size_t i = 0; i < ShadowCascadeCount; ++i) {
            cascades[i] = ComputeCascade(frame, start, ShadowCascadeEnds[i], resolution, casterDistance);
            start = ShadowCascadeEnds[i];
        }
        return cascades;
    }

    std::array<ShadowCascade, TerrainShadowCascadeCount> ComputeTerrainShadowCascades(const Camera& camera, float aspect,
                                                                                      const Math::Vector3f& toSun,
                                                                                      std::uint32_t resolution, float margin) {
        const LightFrame frame = MakeLightFrame(camera, aspect, toSun);
        std::array<ShadowCascade, TerrainShadowCascadeCount> cascades{};
        for (std::size_t i = 0; i < TerrainShadowCascadeCount; ++i) {
            // Centrées sur la caméra, pas devant elle : la tranche se choisit par la distance (SunVisibility), une carte
            // ne dépend ainsi que de la position de la caméra, jamais de son regard (tourner la tête faisait grandir
            // et rétrécir les ombres des montagnes avec les obstacles retenus).
            cascades[i] = CascadeAround(frame, 0.0f, TerrainShadowCascadeEnds[i] * margin, resolution, TerrainShadowCasterDistance);
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

    ShadowReceivers::ShadowReceivers(const Camera& camera, float aspect, const Math::Vector3f& toSun)
        : m_forward(camera.GetForward()), m_toSun(Math::Normalize(toSun)) {
        // Champ un peu élargi : les deux cascades lointaines servent deux images (MeshRenderer::ChooseShadowCascades),
        // la caméra peut avoir tourné entre-temps.
        const float tanY = std::tan(camera.verticalFov * 0.5f) * 1.1f + 0.05f;
        const float tanX = std::tan(camera.verticalFov * 0.5f) * aspect * 1.1f + 0.05f;
        const Math::Vector3f right = camera.GetRight();
        const Math::Vector3f up = camera.GetUp();
        // Distance signée au bord (dehors si positive) : (côté - profondeur x tan) / sqrt(1 + tan²).
        const float scaleX = 1.0f / std::sqrt(1.0f + tanX * tanX);
        const float scaleY = 1.0f / std::sqrt(1.0f + tanY * tanY);
        m_sides = {(right - m_forward * tanX) * scaleX, (-right - m_forward * tanX) * scaleX,
                   (up - m_forward * tanY) * scaleY, (-up - m_forward * tanY) * scaleY};
    }

    bool ShadowReceivers::Reaches(const Math::Vector3f& center, float radius, float length, std::size_t cascade) const {
        return ReachesRange(center, radius, length, ShadowCascadeStart(cascade), ShadowCascadeEnds[cascade]);
    }

    bool ShadowReceivers::ReachesRange(const Math::Vector3f& center, float radius, float length, float sliceStart,
                                       float sliceStop) const {
        // L'ombre balaie le segment [center, end] : elle manque la tranche si les deux bouts sont du même côté
        // extérieur d'un de ses plans (test suffisant, pas exact dans les coins : prudent).
        const Math::Vector3f end = center - m_toSun * length;
        for (const Math::Vector3f& side : m_sides) {
            if (Math::Dot(center, side) > radius && Math::Dot(end, side) > radius) {
                return false;
            }
        }
        // Un mètre de marge : le filtre de l'ombre lit autour du point, et une tranche gardée suit la caméra.
        const float start = sliceStart - radius - 1.0f;
        const float stop = sliceStop + radius + 1.0f;
        const float depthA = Math::Dot(center, m_forward);
        const float depthB = Math::Dot(end, m_forward);
        return std::max(depthA, depthB) >= start && std::min(depthA, depthB) <= stop;
    }
}
