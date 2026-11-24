#pragma once

#include "Flux/Assets/Model.hpp"
#include "Flux/SimExport.hpp"
#include "Flux/Math/Matrix4.hpp"
#include "Flux/Math/Quaternion.hpp"
#include "Flux/Math/Vector3.hpp"

#include <cstdint>
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

    // Nœud de ce nom, sinon du même nom sans l'espace de noms d'un logiciel (« mixamorig:Hips » vaut « Hips ») ; -1
    // s'il n'y en a pas. Pour les noms tapés à la main (masques, os d'accroche) et le recâblage des animations.
    FLUX_SIM_API int FindNode(const ModelSkeleton& skeleton, std::string_view name);

    // Animation du fichier par son nom ; nom vide : la première. nullptr s'il n'y en a pas.
    FLUX_SIM_API const ModelAnimation* FindAnimation(const ModelSkeleton& skeleton, std::string_view name);

    // Instant d'une animation : ramené dans sa durée (boucle), ou borné à ses extrémités.
    FLUX_SIM_API float AnimationTime(const ModelAnimation& animation, double time, bool loop);

    // Une animation à un instant, et sa part dans un mélange ; animation nulle : la pose de repos.
    struct AnimationSample
    {
        const ModelAnimation* animation = nullptr;
        double time = 0.0; // secondes
        bool loop = true;
        float weight = 1.0f;
        std::uint8_t layer = 0; // couche du mélange (EvaluateLayers)
    };

    // Couche d'un mélange, par-dessus les couches d'avant : les os de son masque (node et ses descendants ; -1 : tout
    // le corps) prennent sa pose pour la part weight.
    struct PoseLayer
    {
        int node = -1;
        float weight = 1.0f;
        // Rotation de node vue du modèle : node prend l'orientation qu'il a dans la couche, quels que soient ses
        // parents dans la pose du dessous (Graph::Layer::modelSpace).
        bool modelSpace = false;
    };

    // Calcule la pose d'un squelette. Garde ses tableaux d'une fois sur l'autre : aucune allocation une fois rodé.
    class FLUX_SIM_API PoseEvaluator {
        public:
        // Pose de sample, puis les matrices des os (monde du modèle x inverse de liaison), une par os, telles que le GPU
        // les applique aux sommets.
        void Evaluate(const ModelSkeleton& skeleton, const AnimationSample& sample) { EvaluateBlend(skeleton, std::span(&sample, 1)); }
        // Mélange de plusieurs animations selon leurs parts (fondus, mélange marche et course) : moyenne pondérée
        // des translations et des échelles, des rotations normalisées (nlerp). Aucune part : la pose de repos.
        void EvaluateBlend(const ModelSkeleton& skeleton, std::span<const AnimationSample> samples) { EvaluateLayers(skeleton, samples, {}); }
        // Mélange par couches : la couche 0 (les échantillons de layer 0) pour tout le corps, puis chaque couche c de
        // layers (échantillons de layer c + 1) sur les os de son masque. Ainsi le haut du corps vise pendant que les
        // jambes marchent.
        void EvaluateLayers(const ModelSkeleton& skeleton, std::span<const AnimationSample> samples, std::span<const PoseLayer> layers);

        std::span<const NodePose> GetLocalPoses() const { return m_poses; }
        // Matrice de chaque nœud dans le repère du modèle.
        std::span<const Math::Matrix4f> GetNodeMatrices() const { return m_nodeMatrices; }
        std::span<const Math::Matrix4f> GetJointMatrices() const { return m_jointMatrices; }

        private:
        std::vector<NodePose> m_poses;
        std::vector<NodePose> m_sample;
        std::vector<NodePose> m_layer;
        std::vector<std::uint8_t> m_masked;
        std::vector<Math::Matrix4f> m_nodeMatrices;
        std::vector<Math::Matrix4f> m_jointMatrices;
    };

    // Position d'un sommet déformée par ses os, comme le fait le GPU (shaders/Mesh.slang).
    FLUX_SIM_API Math::Vector3f SkinPosition(std::span<const Math::Matrix4f> jointMatrices, const ModelSkinVertex& skin,
                                         const Math::Vector3f& position);

    // Matrice d'une transformation : translation x rotation x échelle.
    FLUX_SIM_API Math::Matrix4f ComposeMatrix(const NodePose& pose);
    // Translation, rotation et échelle d'une matrice sans cisaillement (celle d'un nœud).
    FLUX_SIM_API NodePose DecomposeMatrix(const Math::Matrix4f& matrix);

    namespace Math {
        // Rotation d'une matrice 3x3 orthonormée (colonnes x, y, z).
        FLUX_SIM_API Quaternionf RotationFromAxes(const Vector3f& x, const Vector3f& y, const Vector3f& z);
    }
}
