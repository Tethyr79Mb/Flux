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
    // (matrices relatives à la caméra, poses des squelettes), puis dessinés dans la passe principale : les opaques
    // d'abord, puis les transparents (matériaux BLEND, ou teinte d'opacité < 1) de l'arrière vers l'avant.
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
            float distance = 0.0f; // du centre de sa boîte à la caméra : ordre des transparents
        };

        // Primitive transparente d'une entité, dessinée après les opaques.
        struct TransparentDraw
        {
            std::uint32_t draw;      // dans m_draws
            std::uint32_t primitive; // dans GpuModel::primitives
        };

        // Pipelines d'un type de sommets (statique ou animé).
        struct Pipelines
        {
            std::unique_ptr<RHI::Pipeline> opaque;            // faces arrière éliminées
            std::unique_ptr<RHI::Pipeline> opaqueDoubleSided;  // faces arrière gardées
            std::unique_ptr<RHI::Pipeline> transparent;        // mélange alpha, profondeur lue mais pas écrite
            std::unique_ptr<RHI::Pipeline> transparentBack;    // idem, faces arrière seules (double face : avant les faces avant)
        };

        // Relie ce qui change d'un appel à l'autre (pipeline, sommets, groupes), seulement quand il le faut.
        struct BindState
        {
            const RHI::Pipeline* pipeline = nullptr;
            const GpuModel* model = nullptr;
            const RHI::BindGroup* group = nullptr;
            bool jointsBound = false;
        };

        explicit MeshRenderer(RHI::Device& device) : m_device(device) {}
        std::expected<void, std::string> CreatePipelines(RHI::Format colorFormat, RHI::Format depthFormat);
        bool GrowJointBuffer(std::uint32_t frameIndex, std::uint64_t size);
        void Pose(const GpuModel& model, const AnimatorComponent* animator);
        void UploadJoints(std::uint32_t frameIndex);
        static bool IsTransparent(const MeshDraw& draw, const GpuMaterial& material);
        void DrawPrimitive(RHI::CommandList& commands, BindState& state, std::uint32_t frameIndex, const MeshDraw& draw,
                           const ModelPrimitive& primitive, const RHI::Pipeline& pipeline) const;

        RHI::Device& m_device;
        std::unique_ptr<ModelCache> m_models;
        Pipelines m_static;
        // Modèles animés : un second flux de sommets (os et poids), et les matrices des os en groupe 1.
        Pipelines m_skinned;
        std::unique_ptr<RHI::BindGroupLayout> m_jointLayout;
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_jointBuffers;
        std::array<std::unique_ptr<RHI::BindGroup>, RHI::FramesInFlight> m_jointGroups;

        // Préparés sur le CPU avant l'enregistrement des commandes, réutilisés d'une image à l'autre.
        std::vector<RelativeTransform> m_relativeTransforms;
        std::vector<MeshDraw> m_draws;
        std::vector<TransparentDraw> m_transparent; // triés du plus loin au plus proche
        std::vector<Math::Matrix4f> m_jointMatrices;
        PoseEvaluator m_poseEvaluator;
    };
}
