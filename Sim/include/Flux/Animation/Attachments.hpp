#pragma once

#include "Flux/SimExport.hpp"

namespace Flux {
    class AnimationLibrary;
    class PoseEvaluator;
    class Scene;

    // Objets accrochés aux os des modèles animés (BoneAttachmentComponent).
    namespace Attachments {
        // Place chaque objet accroché sur l'os de son parent, dans la pose que donnent les animations pondérées de
        // l'animateur du parent (après Animator::Update). La pose d'un parent n'est calculée que s'il porte un objet,
        // une fois par parent quand ses objets se suivent. evaluator : mémoire de travail, gardée d'un appel à l'autre.
        FLUX_SIM_API void Update(Scene& scene, AnimationLibrary& library, PoseEvaluator& evaluator);
    }
}
