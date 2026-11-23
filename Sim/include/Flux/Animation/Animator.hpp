#pragma once

#include "Flux/SimExport.hpp"

#include <string_view>

namespace Flux {
    class AnimationLibrary;
    class PoseEvaluator;
    class Scene;
    struct ModelSkeleton;
    struct AnimationSet;
    struct AnimationGraph;
    struct AnimatorComponent;

    // Lecture des animations des modèles : AnimatorComponent et son graphe. La simulation choisit les animations et
    // leurs parts (aussi sur le serveur) ; la pose en est tirée par Pose, au rendu pour chaque personnage vu et dans
    // la simulation pour ceux qui portent un objet accroché à un os (Attachments).
    namespace Animator {
        // Pose les paramètres des personnages (AnimationParameters), suit les transitions des graphes, avance les
        // lectures et écrit les animations pondérées de chaque AnimatorComponent.
        FLUX_SIM_API void Update(Scene& scene, AnimationLibrary& library, double deltaSeconds);

        // Pose d'un modèle d'après les animations pondérées de son animateur, couche par couche ; sans animateur, la
        // pose de repos ; rien de joué encore (éditeur, avant le premier pas), la première image de son animation.
        // skeleton doit être celui du fichier lu par la simulation (mêmes os) ; set, ses animations avec celles des
        // fichiers du graphe (AnimationLibrary::FindAnimationSet), nullptr : celles du modèle seules.
        FLUX_SIM_API void Pose(PoseEvaluator& evaluator, const ModelSkeleton& skeleton, const AnimationSet* set,
                               const AnimatorComponent* animator);

        // Valeur d'un paramètre déclaré par le graphe de l'animateur, pour les pas suivants ; faux s'il n'y est pas.
        FLUX_SIM_API bool SetParameter(AnimatorComponent& animator, const AnimationGraph& graph, std::string_view name, float value);

        // Durée de l'état portant cette étiquette dans le graphe de l'animateur (sa première animation, à sa vitesse
        // et à celle de l'animateur), secondes ; 0 sans graphe, sans cet état ou sans cette animation.
        FLUX_SIM_API float GetTagDuration(AnimationLibrary& library, const AnimatorComponent& animator, std::string_view model,
                                          std::string_view tag);
    }
}
