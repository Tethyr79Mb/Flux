#include "Flux/Animation/Attachments.hpp"

#include "Flux/Animation/Animation.hpp"
#include "Flux/Animation/AnimationLibrary.hpp"
#include "Flux/Animation/Animator.hpp"
#include "Flux/Scene/Components.hpp"
#include "Flux/Scene/Scene.hpp"

#include <algorithm>

namespace Flux::Attachments {
    void Update(Scene& scene, AnimationLibrary& library, PoseEvaluator& evaluator) {
        Entity posed = NullEntity; // parent dont evaluator tient la pose
        for (const auto [entity, attachment, transform] : scene.GetRegistry().view<const BoneAttachmentComponent, TransformComponent>().each()) {
            const Entity parent = scene.GetParent(entity);
            const auto* model = parent != NullEntity ? scene.TryGet<ModelComponent>(parent) : nullptr;
            const ModelSkeleton* skeleton = model ? library.GetSkeleton(model->model) : nullptr;
            if (!skeleton) {
                continue;
            }
            const auto bone = std::ranges::find(skeleton->nodes, attachment.bone, &SkeletonNode::name);
            if (bone == skeleton->nodes.end()) {
                continue; // os mal nommé : l'objet reste où il est
            }
            if (parent != posed) {
                const auto* animator = scene.TryGet<AnimatorComponent>(parent);
                Animator::Pose(evaluator, *skeleton, library.GetAnimationSet(model->model, animator ? std::string_view(animator->graph) : ""), animator);
                posed = parent;
            }
            // Le repère du modèle est celui de son entité : la transformation locale de l'enfant y est exprimée.
            const NodePose node = DecomposeMatrix(evaluator.GetNodeMatrices()[static_cast<std::size_t>(bone - skeleton->nodes.begin())]);
            transform.position = Math::Vector3d(node.translation + node.rotation.Rotate(attachment.position));
            transform.rotation = node.rotation * attachment.rotation;
        }
    }
}
