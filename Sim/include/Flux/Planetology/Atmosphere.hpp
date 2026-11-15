#pragma once

#include "Flux/SimExport.hpp"
#include "Flux/Math/Vector3.hpp"

#include <array>

namespace Flux {
    class JobSystem;
    struct PlanetComponent;
}

namespace Flux::Planetology {
    // Atmosphère d'une planète : diffusion de Rayleigh (le bleu du ciel, le rouge du couchant, par les molécules) et
    // de Mie (la brume blanchâtre autour du soleil, par les poussières), calculée en marchant le long du regard ; les
    // diffusions suivantes lues dans une table (AtmosphereMultipleTable). Les mêmes calculs dans shaders/Atmosphere.slang (ciel et ce qu'on voit à travers) : toute
    // modification des deux côtés. Ici : la lumière ambiante du ciel, et les tests.
    //
    // Unités de l'éclairage du moteur : une surface blanche face au soleil renvoie la lumière du soleil telle quelle
    // (pas de division par pi) ; la lumière diffusée est ramenée à la même échelle.
    struct AtmosphereParameters
    {
        float radius = 0.0f;        // m, du sol (niveau zéro du relief)
        float top = 0.0f;           // m, rayon du haut de l'atmosphère (au-delà, plus rien)
        Math::Vector3f rayleigh;    // diffusion de Rayleigh au sol (1/m), par couleur
        float rayleighFalloff = 0.0f; // 1 / hauteur d'échelle (1/m) : la densité divisée par e tous les 1/falloff mètres
        float mie = 0.0f;           // diffusion de Mie au sol (1/m)
        float mieFalloff = 0.0f;
        float mieAnisotropy = 0.76f; // g de Henyey-Greenstein : la lumière continue surtout dans sa direction
        float intensity = 1.0f;     // multiplie la lumière diffusée

        bool operator==(const AtmosphereParameters&) const = default;
    };

    // Paramètres d'une planète ; top à 0 sans atmosphère (densité nulle).
    FLUX_SIM_API AtmosphereParameters MakeAtmosphereParameters(const PlanetComponent& planet);

    // Pas de marche le long du regard : ciel, et ce qui est vu à travers (moins : calculé pour chaque pixel des objets).
    inline constexpr int AtmosphereSkySteps = 16;
    inline constexpr int AtmosphereSurfaceSteps = 8;

    // Diffusion multiple (Hillaire 2020, « A Scalable and Production Ready Sky and Atmosphere Rendering Technique ») :
    // la lumière diffusée deux fois et plus, supposée isotrope, ne dépend que de la hauteur et du cosinus du soleil sur
    // la verticale du lieu. Entrée [ligne x taille + colonne] : ligne, hauteur de 0 (sol) à 1 (haut de l'air) ; colonne,
    // cosinus de -1 à 1. Par unité de lumière du soleil, à multiplier par la diffusion du lieu (1/m). Ne dépend que de
    // l'air (pas du soleil) : calculée une fois par atmosphère, envoyée telle quelle au GPU (shaders/Atmosphere.slang).
    // Environ 35 ms sur un cœur, réparties sur les fils de `jobs` s'il y en a.
    inline constexpr int AtmosphereMultipleSize = 32;
    using AtmosphereMultipleTable = std::array<Math::Vector3f, AtmosphereMultipleSize * AtmosphereMultipleSize>;
    FLUX_SIM_API AtmosphereMultipleTable ComputeAtmosphereMultiple(const AtmosphereParameters& atmosphere, JobSystem* jobs = nullptr);

    // Transmittance (part de la lumière qui passe, par couleur) du point `position` (relatif au centre de la planète)
    // jusqu'au soleil ; nulle si la planète le cache (côté nuit).
    FLUX_SIM_API Math::Vector3f SunTransmittance(const AtmosphereParameters& atmosphere, const Math::Vector3f& position,
                                            const Math::Vector3f& toSun);

    // Lumière diffusée vers l'œil, placé en `origin` (relatif au centre) et regardant dans `direction` (unitaire),
    // sur `distance` mètres au plus (le sol ou un objet ; très grand pour le ciel), pour un soleil `sunColor`.
    // transmittance : ce qui passe de la lumière venue de plus loin (le ciel derrière, l'objet). multiple : la table de
    // la diffusion multiple de cet air (nulle : une seule diffusion).
    FLUX_SIM_API Math::Vector3f Scatter(const AtmosphereParameters& atmosphere, const Math::Vector3f& origin,
                                    const Math::Vector3f& direction, float distance, const Math::Vector3f& toSun,
                                    const Math::Vector3f& sunColor, int steps, Math::Vector3f& transmittance,
                                    const AtmosphereMultipleTable* multiple = nullptr);

    // Lumière ambiante du ciel au sol (moyenne du ciel pondérée par le cosinus, pour une surface tournée vers le haut,
    // sans le soleil lui-même), selon la hauteur du soleil : entrée i pour un cosinus de -1 + 2 i / (taille - 1)
    // entre la verticale du lieu et le soleil. Le shader lit ce tableau pour chaque pixel, sans tout recalculer.
    inline constexpr int AtmosphereAmbientSize = 16;
    FLUX_SIM_API std::array<Math::Vector3f, AtmosphereAmbientSize> ComputeAtmosphereAmbient(const AtmosphereParameters& atmosphere,
                                                                                       const Math::Vector3f& sunColor,
                                                                                       const AtmosphereMultipleTable* multiple = nullptr);
}
