#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Matrix4.hpp"
#include "Flux/Math/Quaternion.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Renderer/Model.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace Flux {
    // Transformation locale d'un nœud à un instant.
    struct NodePose
    {
        Math::Vector3f translation;
        Math::Quaternionf rotation;
        Math::Vector3f scale{1.0f, 1.0f, 1.0f};
    };

    // Animation du fichier par son nom ; nom vide : la première. nullptr s'il n'y en a pas.
    FLUX_API const ModelAnimation* FindAnimation(const ModelSkeleton& skeleton, std::string_view name);

    // Instant d'une animation : ramené dans sa durée (boucle), ou borné à ses extrémités.
    FLUX_API float AnimationTime(const ModelAnimation& animation, double time, bool loop);

    // Une animation à un instant ; animation nulle : la pose de repos.
    struct AnimationSample
    {
        const ModelAnimation* animation = nullptr;
        double time = 0.0; // secondes
        bool loop = true;
    };

    // Calcule la pose d'un squelette. Garde ses tableaux d'une fois sur l'autre : aucune allocation une fois rodé.
    class FLUX_API PoseEvaluator {
        public:
        // Pose de sample, puis les matrices des os (monde du modèle x inverse de liaison), une par os, telles que le GPU
        // les applique aux sommets.
        void Evaluate(const ModelSkeleton& skeleton, const AnimationSample& sample) { Evaluate(skeleton, sample, sample, 1.0f); }
        // Fondu de from vers to : weight 0 donne from, 1 donne to (changement d'animation en douceur).
        void Evaluate(const ModelSkeleton& skeleton, const AnimationSample& from, const AnimationSample& to, float weight);

        std::span<const NodePose> GetLocalPoses() const { return m_poses; }
        // Matrice de chaque nœud dans le repère du modèle.
        std::span<const Math::Matrix4f> GetNodeMatrices() const { return m_nodeMatrices; }
        std::span<const Math::Matrix4f> GetJointMatrices() const { return m_jointMatrices; }

        private:
        std::vector<NodePose> m_poses;
        std::vector<NodePose> m_from;
        std::vector<Math::Matrix4f> m_nodeMatrices;
        std::vector<Math::Matrix4f> m_jointMatrices;
    };

    // Position d'un sommet déformée par ses os, comme le fait le GPU (shaders/Mesh.slang).
    FLUX_API Math::Vector3f SkinPosition(std::span<const Math::Matrix4f> jointMatrices, const ModelSkinVertex& skin,
                                         const Math::Vector3f& position);

    // Matrice d'une transformation : translation x rotation x échelle.
    FLUX_API Math::Matrix4f ComposeMatrix(const NodePose& pose);
}
