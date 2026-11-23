#pragma once

#include "Flux/SimExport.hpp"

#include <string_view>

namespace Flux {
    class AnimationLibrary;
    class Scene;
    struct AnimationGraph;
    struct AnimatorComponent;

    // Lecture des animations des modèles : AnimatorComponent et son graphe. La simulation choisit les animations et
    // leurs parts (aussi sur le serveur) ; la pose elle-même est calculée au rendu, d'après AnimatorComponent::clips.
    namespace Animator {
        // Pose les paramètres des personnages (AnimationParameters), suit les transitions des graphes, avance les
        // lectures et écrit les animations pondérées de chaque AnimatorComponent.
        FLUX_SIM_API void Update(Scene& scene, AnimationLibrary& library, double deltaSeconds);

        // Valeur d'un paramètre déclaré par le graphe de l'animateur, pour les pas suivants ; faux s'il n'y est pas.
        FLUX_SIM_API bool SetParameter(AnimatorComponent& animator, const AnimationGraph& graph, std::string_view name, float value);

        // Durée de l'état portant cette étiquette dans le graphe de l'animateur (sa première animation, à sa vitesse
        // et à celle de l'animateur), secondes ; 0 sans graphe, sans cet état ou sans cette animation.
        FLUX_SIM_API float GetTagDuration(AnimationLibrary& library, const AnimatorComponent& animator, std::string_view model,
                                          std::string_view tag);
    }
}
