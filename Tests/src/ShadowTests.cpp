// Tests des cascades d'ombres : chaque tranche de la vue tient dans sa carte, calée sur la grille des texels du monde.
#include "Check.hpp"

#include "Flux/Graphics/Camera.hpp"
#include "Flux/Graphics/ShadowCascades.hpp"

#include <cmath>

using namespace Flux;

namespace {
    Math::Vector3f Apply(const Math::Matrix4f& m, const Math::Vector3f& p) {
        const auto& c = m.columns;
        return {c[0][0] * p.x + c[1][0] * p.y + c[2][0] * p.z + c[3][0],
                c[0][1] * p.x + c[1][1] * p.y + c[2][1] * p.z + c[3][1],
                c[0][2] * p.x + c[1][2] * p.y + c[2][2] * p.z + c[3][2]};
    }

    bool Inside(const Math::Vector3f& p) {
        constexpr float eps = 1.0e-4f;
        return std::abs(p.x) <= 1.0f + eps && std::abs(p.y) <= 1.0f + eps && p.z >= -eps && p.z <= 1.0f + eps;
    }

    // Distance à l'entier le plus proche, en fraction de texel.
    double GridError(double value, double texel) {
        const double t = value / texel;
        return std::abs(t - std::round(t));
    }
}

bool RunShadowTests() {
    const Math::Vector3f toSun = Math::Normalize(Math::Vector3f{0.36f, 0.89f, 0.27f});
    constexpr float aspect = 16.0f / 9.0f;
    constexpr std::uint32_t resolution = 2048;

    Camera camera;
    camera.position.offset = {1234.567, 89.1, -4321.25};
    camera.yaw = 0.7f;
    camera.pitch = -0.3f;
    const auto cascades = ComputeShadowCascades(camera, aspect, toSun, resolution);

    // Les 8 coins de chaque tranche de la vue tombent dans la carte de leur cascade.
    const Math::Vector3f forward = camera.GetForward();
    const Math::Vector3f right = camera.GetRight();
    const Math::Vector3f up = camera.GetUp();
    const float tanY = std::tan(camera.verticalFov * 0.5f);
    const float tanX = tanY * aspect;
    float start = camera.nearPlane;
    for (int i = 0; i < ShadowCascadeCount; ++i) {
        const ShadowCascade& cascade = cascades[static_cast<std::size_t>(i)];
        const float end = ShadowCascadeEnds[static_cast<std::size_t>(i)];
        for (const float depth : {start, end}) {
            for (const float sx : {-1.0f, 1.0f}) {
                for (const float sy : {-1.0f, 1.0f}) {
                    const Math::Vector3f corner = forward * depth + right * (sx * tanX * depth) + up * (sy * tanY * depth);
                    CHECK(Inside(Apply(cascade.lightViewProjection, corner)));
                }
            }
        }
        // Un obstacle à casterDistance (300 m) vers le soleil est encore vu, tout au bord côté soleil.
        const Math::Vector3f caster = Apply(cascade.lightViewProjection, cascade.center + toSun * 300.0f);
        CHECK(std::abs(caster.z) < 1.0e-3f);
        CHECK(std::abs(caster.x) < 1.0e-3f && std::abs(caster.y) < 1.0e-3f);
        CHECK(std::abs(cascade.texelSize * static_cast<float>(resolution) - 2.0f * cascade.radius) < 1.0e-3f);
        start = end;
    }
    // Plus loin, plus grand : les cascades couvrent des zones croissantes.
    for (int i = 1; i < ShadowCascadeCount; ++i) {
        CHECK(cascades[static_cast<std::size_t>(i)].radius > cascades[static_cast<std::size_t>(i - 1)].radius);
    }

    // Carte gardée d'une image à l'autre (MoveShadowCascade) : un point fixe du monde tombe au même endroit de la carte
    // vu de la caméra déplacée.
    {
        const Math::Vector3f offset{3.5f, -0.25f, 12.0f};
        const ShadowCascade moved = MoveShadowCascade(cascades[3], offset);
        const Math::Vector3f point{40.0f, -2.0f, -90.0f}; // relatif à l'ancienne caméra
        const Math::Vector3f before = Apply(cascades[3].lightViewProjection, point);
        const Math::Vector3f after = Apply(moved.lightViewProjection, point - offset);
        CHECK(std::abs(before.x - after.x) < 1.0e-5f && std::abs(before.y - after.y) < 1.0e-5f
              && std::abs(before.z - after.z) < 1.0e-5f);
        CHECK(std::abs(moved.center.z - (cascades[3].center.z - offset.z)) < 1.0e-4f);
    }

    // Tourner la caméra ne change pas la taille des cascades (pas de scintillement des bords d'ombre).
    Camera turned = camera;
    turned.yaw += 1.1f;
    turned.pitch = 0.4f;
    const auto turnedCascades = ComputeShadowCascades(turned, aspect, toSun, resolution);
    for (int i = 0; i < ShadowCascadeCount; ++i) {
        CHECK(turnedCascades[static_cast<std::size_t>(i)].radius == cascades[static_cast<std::size_t>(i)].radius);
    }

    // Le centre de chaque cascade, en coordonnées du monde, est sur la grille des texels : un déplacement de la
    // caméra fait glisser la carte d'un nombre entier de texels.
    for (const Camera& c : {camera, turned}) {
        const auto list = ComputeShadowCascades(c, aspect, toSun, resolution);
        for (const ShadowCascade& cascade : list) {
            // Axes de la carte, calculés comme ComputeShadowCascades (le soleil n'est pas vertical).
            const Math::Vector3f lightForward = toSun * -1.0f;
            const Math::Vector3f lightRight = Math::Normalize(Math::Cross(lightForward, Math::Vector3f{0.0f, 1.0f, 0.0f}));
            const Math::Vector3d mapRight(lightRight);
            const Math::Vector3d mapUp(Math::Cross(lightRight, lightForward));
            const Math::Vector3d world = c.position.offset + Math::Vector3d(cascade.center);
            const double texel = cascade.texelSize;
            CHECK(GridError(Math::Dot(world, mapRight), texel) < 0.02);
            CHECK(GridError(Math::Dot(world, mapUp), texel) < 0.02);
        }
    }

    // Obstacles écartés des cartes : seulement ceux dont l'ombre ne peut tomber nulle part dans la tranche vue.
    const ShadowReceivers receivers(camera, aspect, toSun);
    const Math::Vector3f ahead = forward * 40.0f; // dans la tranche 2 (20 à 60 m)
    CHECK(receivers.Reaches(ahead, 1.0f, 30.0f, 2));
    CHECK(!receivers.Reaches(ahead, 1.0f, 0.0f, 0) && !receivers.Reaches(ahead, 1.0f, 0.0f, 3));
    // Derrière la caméra : gardé si le soleil est derrière aussi (l'ombre revient devant), écarté s'il est devant.
    const Math::Vector3f behind = forward * -30.0f;
    CHECK(ShadowReceivers(camera, aspect, -forward).Reaches(behind, 1.0f, 300.0f, 1));
    CHECK(!ShadowReceivers(camera, aspect, forward).Reaches(behind, 1.0f, 300.0f, 1));
    // Ombre trop courte pour arriver jusqu'à la tranche.
    CHECK(!ShadowReceivers(camera, aspect, -forward).Reaches(behind, 1.0f, 10.0f, 1));
    // Sur le côté, hors du champ, ombre courte : écarté ; une ombre assez longue pour rentrer dans le champ : gardé.
    const Math::Vector3f beside = forward * 30.0f + right * 200.0f;
    CHECK(!receivers.Reaches(beside, 2.0f, 5.0f, 2));
    const ShadowReceivers sideSun(camera, aspect, right);
    CHECK(sideSun.Reaches(beside, 2.0f, 250.0f, 2));
    return true;
}
