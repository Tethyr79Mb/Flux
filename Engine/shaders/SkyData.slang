// Données du ciel pour une image, partagées par le fond (Sky.slang) et le soleil (Sun.slang).

#include "Atmosphere.slang"

// Doit correspondre à SkyData dans SkyRenderer.cpp.
struct SkyData
{
    float4 right;        // xyz : droite de la caméra x tangente du demi-champ horizontal
    float4 up;           // xyz : haut de la caméra x tangente du demi-champ vertical
    float4 forward;      // xyz : avant de la caméra ; w : angle couvert par un pixel (radians)
    float4 toSun;        // xyz : direction du soleil (unitaire) ; w : rayon apparent du disque (radians)
    float4 sunColor;     // rgb : couleur de la lumière x intensité ; w : exposition
    float4 discColor;    // rgb : couleur du disque x intensité
    float4 nebulaColor;  // rgb ; w : densité de la nébuleuse
    float4 nebulaColor2; // rgb ; w : éclat de la bande galactique
    float4 galaxyAxis;   // xyz ; w : quantité d'étoiles
    float4 galaxyCenter; // xyz ; w : éclat des étoiles
    float4 noiseOffset;  // xyz ; w : luminosité du fond
    uint4 options;       // x : graine mélangée ; y : 1 panorama, 0 ciel généré ; z : rayons du soleil (SunRays)
    AtmosphereData atmosphere;
};

[[vk::binding(0, 0)]] StructuredBuffer<SkyData> sky;
