#pragma once

#include <cmath>
#include <numbers>

// Unités physiques des composants (lux, lumens, cd/m²) et unité de lumière des shaders. Les shaders gardent l'échelle
// d'avant les unités physiques : un éclairement de 1 vaut LuxPerUnit (le plein soleil d'alors, 1,45 = 116 000 lux) ;
// une surface blanche sous cet éclairement renvoie 1 (le 1/π du diffus est compris dans l'unité). Ainsi les valeurs
// gardent la précision des textures fp16 qui ne sont pas pré-exposées (environnement, tables de l'atmosphère), et les
// effets réglés à l'œil sur cette échelle (flammes, traceurs) n'ont pas bougé. La conversion se fait une fois, sur le
// CPU, en préparant les données des shaders.
namespace Flux::Photometry {
    inline constexpr float LuxPerUnit = 80000.0f;
    // Luminance d'une surface blanche éclairée par LuxPerUnit : l'unité de lumière renvoyée vers l'œil.
    inline constexpr float NitsPerUnit = LuxPerUnit / std::numbers::pi_v<float>;
    // Émissif des modèles glTF (facteur x KHR_materials_emissive_strength) : 1 vaut 1000 cd/m² (un écran au plus
    // clair ; un néon 5000 à 20 000).
    inline constexpr float NitsPerGltfEmissive = 1000.0f;

    // Éclairement (lux) -> unité des shaders.
    constexpr float FromLux(float lux) { return lux / LuxPerUnit; }
    // Luminance (cd/m²) -> unité des shaders.
    constexpr float FromNits(float nits) { return nits / NitsPerUnit; }
    // Flux d'une lumière ponctuelle (lumens) -> intensité dans l'unité des shaders : l'éclairement à 1 m. Un spot
    // garde l'intensité de la même ampoule nue (comme Unreal) : fermer son cône n'en change pas l'éclat.
    constexpr float FromLumens(float lumens) { return FromLux(lumens / (4.0f * std::numbers::pi_v<float>)); }

    // Exposition (ce qui multiplie la lumière des shaders) d'un appareil réglé à `ev100` (EV à 100 ISO) : la
    // convention de Lagarde (Frostbite, 2014), H = 1 / (1,2 x 2^EV100) sur la luminance en cd/m². Repères : plein soleil
    // 15, ciel couvert 12, intérieur éclairé 7, nuit de pleine lune -2.
    inline float ExposureFromEv100(float ev100) { return NitsPerUnit / (1.2f * std::exp2(ev100)); }
}
