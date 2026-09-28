#pragma once

#include "Flux/Export.hpp"

namespace Flux {
    class Scene;

    // Lecture des animations des modèles : AnimatorComponent. La pose elle-même est calculée au rendu, d'après
    // l'état laissé ici (animation, instant, fondu).
    namespace Animator {
        // Durée du fondu d'une animation à l'autre, en secondes.
        inline constexpr float FadeSeconds = 0.25f;
        // Vitesse sous laquelle un personnage est considéré immobile, m/s.
        inline constexpr float IdleSpeed = 0.1f;
        // Temps en l'air avant de jouer l'animation « en l'air » : une marche d'escalier ou une bosse ne la
        // déclenche pas, en secondes.
        inline constexpr float AirDelay = 0.15f;

        // Choisit l'animation de chaque AnimatorComponent (celle des pas d'un personnage) et avance sa lecture.
        FLUX_API void Update(Scene& scene, double deltaSeconds);
    }
}
