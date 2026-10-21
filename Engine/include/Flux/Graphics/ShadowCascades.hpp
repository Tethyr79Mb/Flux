#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Matrix4.hpp"
#include "Flux/Math/Vector3.hpp"

#include <array>
#include <cstdint>

namespace Flux {
    struct Camera;

    // Ombres du soleil en cascades : la vue est découpée en tranches de plus en plus lointaines, chacune couverte par
    // sa propre carte d'ombres (même résolution) : précises tout près, grossières au loin.
    inline constexpr int ShadowCascadeCount = 4;
    // Fin de chaque tranche, en mètres devant la caméra ; au-delà de la dernière, pas d'ombre.
    inline constexpr std::array<float, ShadowCascadeCount> ShadowCascadeEnds{6.0f, 20.0f, 60.0f, 200.0f};

    struct ShadowCascade
    {
        // Position relative à la caméra -> x, y dans [-1, 1] sur la carte, profondeur dans [0, 1] (0 : côté soleil).
        Math::Matrix4f lightViewProjection;
        Math::Vector3f center; // relatif à la caméra
        float radius = 0.0f;   // demi-côté du carré couvert, en mètres
        float texelSize = 0.0f; // côté d'un texel de la carte, en mètres
        float depthRange = 0.0f; // profondeur couverte le long des rayons du soleil, en mètres
    };

    // Cascades de la caméra pour un soleil dans la direction `toSun` (unitaire, vers le soleil), cartes de
    // `resolution` texels de côté. Chaque tranche est enfermée dans une sphère (taille fixe quand la caméra tourne) et
    // son centre est calé sur la grille des texels du monde : les bords d'ombre ne scintillent pas quand la caméra
    // bouge. Les obstacles jusqu'à `casterDistance` mètres vers le soleil projettent leur ombre dans la tranche.
    FLUX_API std::array<ShadowCascade, ShadowCascadeCount> ComputeShadowCascades(const Camera& camera, float aspect,
                                                                                 const Math::Vector3f& toSun,
                                                                                 std::uint32_t resolution,
                                                                                 float casterDistance = 300.0f);

    // La même cascade (même carte) vue d'une caméra déplacée de `cameraOffset` (nouvelle position - ancienne, en
    // mètres) : sa matrice prend les positions relatives à la nouvelle caméra. Sert à garder la carte d'une image
    // précédente.
    FLUX_API ShadowCascade MoveShadowCascade(const ShadowCascade& cascade, const Math::Vector3f& cameraOffset);
}
