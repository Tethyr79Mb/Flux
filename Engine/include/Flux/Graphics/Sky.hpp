#pragma once

#include "Flux/Assets/Image.hpp"
#include "Flux/Export.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Vector3.hpp"

#include <cstdint>
#include <vector>

namespace Flux {
    struct SkyComponent;

    // Ciel généré : ce qui le décrit, tiré d'un SkyComponent. Calculé une fois sur le CPU (vecteurs tirés de la
    // graine) et envoyé tel quel au shader du ciel (shaders/Sky.slang), qui refait le même calcul au pixel : le fond
    // net vient du shader, la texture cube de la lumière ambiante et des reflets vient d'ici.
    struct SkyParameters
    {
        std::uint32_t seed = 0;         // graine mélangée (jamais la valeur brute)
        Math::Vector3f noiseOffset;     // décalage du bruit : une autre nébuleuse pour une autre graine
        Math::Vector3f galaxyAxis;      // perpendiculaire au plan de la galaxie (unitaire)
        Math::Vector3f galaxyCenter;    // direction du centre de la galaxie, dans son plan (unitaire)
        Math::LinearColor nebulaColor;
        Math::LinearColor nebulaColor2;
        float nebula = 0.0f; // densité, 0 à 1
        float galaxy = 0.0f; // éclat de la bande, 0 à 1

        bool operator==(const SkyParameters&) const = default;
    };

    FLUX_API SkyParameters MakeSkyParameters(const SkyComponent& sky);

    // Fond du ciel généré (nébuleuse et bande galactique, sans les étoiles), en linéaire, avant la luminosité, dans
    // la direction unitaire `direction`. Doit rester identique à SkyBackground dans shaders/Sky.slang.
    FLUX_API Math::Vector3f SkyBackground(const Math::Vector3f& direction, const SkyParameters& sky);

    // Direction vers le soleil (unitaire) : hauteur au-dessus de l'horizon et direction autour de la verticale
    // (0 : vers +z, 90 : vers +x), en degrés.
    FLUX_API Math::Vector3f SunDirection(float elevationDegrees, float azimuthDegrees);

    // Direction (unitaire) d'un point d'une face de texture cube, dans l'ordre et l'orientation de Vulkan : faces +X,
    // -X, +Y, -Y, +Z, -Z ; u vers la droite et v vers le bas de la face, de -1 à 1.
    FLUX_API Math::Vector3f CubeFaceDirection(int face, float u, float v);

    // Textures cube (6 faces carrées de `size` pixels à la suite, RGBA 8 bits sRGB), pour RHI::CreateCubeTexture.
    // Ciel généré (sans les étoiles, trop fines pour la lumière qu'elles donnent).
    FLUX_API std::vector<std::uint8_t> GenerateSkyCube(const SkyParameters& sky, std::uint32_t size);
    // Panorama 2:1 (projection équirectangulaire : longitude en largeur, du haut au bas du ciel en hauteur), chaque
    // texel moyenné sur plusieurs points de l'image.
    FLUX_API std::vector<std::uint8_t> PanoramaToCube(const ImageData& panorama, std::uint32_t size);
    // Dégradé du sol (en bas) au ciel (en haut) : la lumière ambiante d'une scène sans ciel.
    FLUX_API std::vector<std::uint8_t> GradientCube(const Math::LinearColor& top, const Math::LinearColor& bottom,
                                                    std::uint32_t size);

    // Point de l'image panoramique (u, v de 0 à 1, v vers le bas) vu dans la direction unitaire `direction`.
    // Doit rester identique à PanoramaUv dans shaders/Sky.slang.
    FLUX_API void PanoramaUv(const Math::Vector3f& direction, float& u, float& v);
}
