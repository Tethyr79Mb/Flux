#pragma once

#include "Flux/Animation/Animation.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Renderer/ModelCache.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Flux {
    struct Camera;

    // Maillages de la scène (cubes et modèles glTF, animés ou non) : préparés sur le CPU pour chaque image
    // (matrices relatives à la caméra, poses des squelettes), puis dessinés dans la passe principale.
    class MeshRenderer {
        public:
        static std::expected<std::unique_ptr<MeshRenderer>, std::string> Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                 RHI::Format depthFormat);

        ModelCache& GetModels() { return *m_models; }

        // Une matrice par maillage visible, calculée relativement à la caméra, et les os des modèles animés envoyés
        // au GPU. `hidden` et ses descendants sont écartés. Après Device::BeginFrame : les ressources de l'image
        // frameIndex sont libres.
        void Prepare(const Scene& scene, const Camera& camera, float aspect, Entity hidden,
                     const std::filesystem::path& assetsDirectory, std::uint32_t frameIndex);
        void Draw(RHI::CommandList& commands, std::uint32_t frameIndex) const;

        private:
        // Une entité à dessiner : son modèle et ses matrices, calculées pour l'image.
        struct MeshDraw
        {
            const GpuModel* model;
            Math::Matrix4f modelViewProjection;
            std::array<std::array<float, 4>, 3> normalColumns;
            Math::LinearColor tint;
            std::uint32_t firstJoint = 0; // modèle animé : ses os dans le buffer des os de l'image
        };

        explicit MeshRenderer(RHI::Device& device) : m_device(device) {}
        std::expected<void, std::string> CreatePipelines(RHI::Format colorFormat, RHI::Format depthFormat);
        bool GrowJointBuffer(std::uint32_t frameIndex, std::uint64_t size);
        void Pose(const GpuModel& model, const AnimatorComponent* animator);
        void UploadJoints(std::uint32_t frameIndex);

        RHI::Device& m_device;
        std::unique_ptr<ModelCache> m_models;
        // Faces arrière éliminées, ou gardées (matériaux double face).
        std::unique_ptr<RHI::Pipeline> m_static;
        std::unique_ptr<RHI::Pipeline> m_staticDoubleSided;
        // Modèles animés : un second flux de sommets (os et poids), et les matrices des os en groupe 1.
        std::unique_ptr<RHI::Pipeline> m_skinned;
        std::unique_ptr<RHI::Pipeline> m_skinnedDoubleSided;
        std::unique_ptr<RHI::BindGroupLayout> m_jointLayout;
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_jointBuffers;
        std::array<std::unique_ptr<RHI::BindGroup>, RHI::FramesInFlight> m_jointGroups;

        // Préparés sur le CPU avant l'enregistrement des commandes, réutilisés d'une image à l'autre.
        std::vector<RelativeTransform> m_relativeTransforms;
        std::vector<MeshDraw> m_draws;
        std::vector<Math::Matrix4f> m_jointMatrices;
        PoseEvaluator m_poseEvaluator;
    };
}
