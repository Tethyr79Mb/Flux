#include "Renderer/MeshRenderer.hpp"

#include "Flux/Graphics/Camera.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <span>

namespace Flux {
    namespace {
        static_assert(sizeof(ModelVertex) == 32, "ModelVertex doit correspondre à VertexInput dans shaders/Mesh.slang");
        static_assert(sizeof(ModelSkinVertex) == 24, "ModelSkinVertex doit correspondre à SkinnedVertexInput dans shaders/Mesh.slang");

        // Doit correspondre à MeshParams dans shaders/Mesh.slang : 128 octets, le minimum garanti des push constants.
        struct MeshParams
        {
            Math::Matrix4f modelViewProjection;
            // normalColumns[0][3] : seuil d'alpha ; normalColumns[1][3] : premier os (bits d'un entier), modèle animé
            std::array<std::array<float, 4>, 3> normalColumns;
            Math::LinearColor color;
        };
        static_assert(sizeof(MeshParams) == 128);

        // Matrices d'os envoyées au GPU par image : de quoi animer des dizaines de personnages sans réallouer.
        constexpr std::uint64_t MinJointBufferSize = 64 * 1024;

        // `entity` est `ancestor` ou l'un de ses descendants.
        bool IsInside(const Scene& scene, Entity entity, Entity ancestor) {
            if (ancestor == NullEntity) {
                return false;
            }
            for (; entity != NullEntity; entity = scene.GetParent(entity)) {
                if (entity == ancestor) {
                    return true;
                }
            }
            return false;
        }
    }

    std::expected<std::unique_ptr<MeshRenderer>, std::string> MeshRenderer::Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                   RHI::Format depthFormat) {
        std::unique_ptr<MeshRenderer> renderer(new MeshRenderer(device));
        auto models = ModelCache::Create(device);
        if (!models) {
            return std::unexpected(models.error());
        }
        renderer->m_models = std::move(*models);

        constexpr RHI::Binding binding{0, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Vertex};
        auto layout = device.CreateBindGroupLayout({.bindings = {&binding, 1}, .maxGroups = RHI::FramesInFlight});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        renderer->m_jointLayout = std::move(*layout);
        for (std::uint32_t f = 0; f < RHI::FramesInFlight; ++f) {
            renderer->m_jointGroups[f] = device.CreateBindGroup(*renderer->m_jointLayout);
            if (!renderer->m_jointGroups[f]) {
                return std::unexpected("allocation des groupes des os");
            }
            if (!renderer->GrowJointBuffer(f, MinJointBufferSize)) {
                return std::unexpected("création du buffer des os");
            }
        }

        if (auto pipelines = renderer->CreatePipelines(colorFormat, depthFormat); !pipelines) {
            return std::unexpected(pipelines.error());
        }
        return renderer;
    }

    std::expected<void, std::string> MeshRenderer::CreatePipelines(RHI::Format colorFormat, RHI::Format depthFormat) {
        const std::array vertexBuffers{
            RHI::VertexBufferLayout{0, sizeof(ModelVertex)},
            RHI::VertexBufferLayout{1, sizeof(ModelSkinVertex)},
        };
        const std::array attributes{
            RHI::VertexAttribute{0, 0, RHI::Format::RGB32Float, offsetof(ModelVertex, position)},
            RHI::VertexAttribute{1, 0, RHI::Format::RGB32Float, offsetof(ModelVertex, normal)},
            RHI::VertexAttribute{2, 0, RHI::Format::RG32Float, offsetof(ModelVertex, uv)},
            // Modèles animés seulement : os et poids, second flux.
            RHI::VertexAttribute{3, 1, RHI::Format::RGBA16Uint, offsetof(ModelSkinVertex, joints)},
            RHI::VertexAttribute{4, 1, RHI::Format::RGBA32Float, offsetof(ModelSkinVertex, weights)},
        };
        const std::array<const RHI::BindGroupLayout*, 2> layouts{&m_models->GetMaterialLayout(), m_jointLayout.get()};

        RHI::PipelineDesc desc;
        desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Mesh.spv";
        desc.vertexBuffers = std::span(vertexBuffers).first(1);
        desc.vertexAttributes = std::span(attributes).first(3);
        desc.bindGroupLayouts = std::span(layouts).first(1);
        desc.pushConstantsSize = sizeof(MeshParams);
        desc.colorFormat = colorFormat;
        desc.depthFormat = depthFormat;
        desc.depthTest = true;
        desc.depthWrite = true;

        const auto create = [&](std::unique_ptr<RHI::Pipeline>& pipeline, RHI::CullMode cullMode) -> std::expected<void, std::string> {
            desc.cullMode = cullMode;
            auto created = m_device.CreatePipeline(desc);
            if (!created) {
                return std::unexpected(created.error());
            }
            pipeline = std::move(*created);
            return {};
        };
        if (auto result = create(m_static, RHI::CullMode::Back); !result) {
            return result;
        }
        if (auto result = create(m_staticDoubleSided, RHI::CullMode::None); !result) {
            return result;
        }

        // Même description, plus les os et poids (flux 1) et les matrices des os (groupe 1, lues par le vertex shader).
        desc.vertexEntryPoint = "vertexSkinned";
        desc.vertexBuffers = vertexBuffers;
        desc.vertexAttributes = attributes;
        desc.bindGroupLayouts = layouts;
        if (auto result = create(m_skinned, RHI::CullMode::Back); !result) {
            return result;
        }
        return create(m_skinnedDoubleSided, RHI::CullMode::None);
    }

    // Buffer des os de l'image f agrandi à size octets au moins, et son groupe mis à jour. Appelé quand le GPU
    // n'utilise plus cette image (après BeginFrame).
    bool MeshRenderer::GrowJointBuffer(std::uint32_t f, std::uint64_t size) {
        std::unique_ptr<RHI::Buffer>& buffer = m_jointBuffers[f];
        if (buffer && buffer->GetSize() >= size) {
            return true;
        }
        auto grown = m_device.CreateBuffer({std::bit_ceil(std::max(size, MinJointBufferSize)), RHI::BufferUsage::Storage,
                                            RHI::MemoryType::Upload});
        if (!grown) {
            std::fprintf(stderr, "Flux : %s\n", grown.error().c_str());
            return false;
        }
        buffer = std::move(*grown);
        m_jointGroups[f]->SetBuffer(0, *buffer);
        return true;
    }

    void MeshRenderer::Prepare(const Scene& scene, const Camera& camera, float aspect, Entity hidden,
                               const std::filesystem::path& assetsDirectory, std::uint32_t frameIndex) {
        // La soustraction des positions se fait en double (secteurs compris) : le GPU ne voit que de petits nombres.
        const Math::Matrix4f viewProjection = camera.GetProjectionMatrix(aspect) * camera.GetViewMatrix();
        scene.ComputeRelativeTransforms(camera.position, m_relativeTransforms);
        m_draws.clear();
        m_jointMatrices.clear();
        for (const RelativeTransform& transform : m_relativeTransforms) {
            const GpuModel* model = nullptr;
            Math::LinearColor tint;
            if (const auto* renderer = scene.TryGet<MeshRendererComponent>(transform.entity)) {
                model = &m_models->GetCube();
                tint = renderer->color;
            } else if (const auto* component = scene.TryGet<ModelComponent>(transform.entity);
                       component && !component->model.empty() && !assetsDirectory.empty()) {
                model = m_models->Find(assetsDirectory / Platform::PathFromUtf8(component->model));
                tint = component->tint;
            }
            if (!model || IsInside(scene, transform.entity, hidden)) {
                continue;
            }
            const Math::Matrix4f world = Math::Matrix4f::Translation(Math::Vector3f(transform.position))
                                       * transform.rotation.ToMatrix()
                                       * Math::Matrix4f::Scale(transform.scale);
            // Normales : rotation x inverse de l'échelle (une échelle non uniforme penche les normales).
            // Échelle nulle (objet aplati) : un grand nombre plutôt qu'une division par zéro.
            const auto inverse = [](float scale) { return std::abs(scale) > 1.0e-12f ? 1.0f / scale : 1.0e12f; };
            const Math::Vector3f x = transform.rotation.Rotate(Math::Vector3f{inverse(transform.scale.x), 0.0f, 0.0f});
            const Math::Vector3f y = transform.rotation.Rotate(Math::Vector3f{0.0f, inverse(transform.scale.y), 0.0f});
            const Math::Vector3f z = transform.rotation.Rotate(Math::Vector3f{0.0f, 0.0f, inverse(transform.scale.z)});
            const std::array<std::array<float, 4>, 3> normalColumns{{{x.x, x.y, x.z, 0.0f},
                                                                     {y.x, y.y, y.z, 0.0f},
                                                                     {z.x, z.y, z.z, 0.0f}}};
            std::uint32_t firstJoint = 0;
            if (!model->skeleton.joints.empty()) {
                firstJoint = static_cast<std::uint32_t>(m_jointMatrices.size());
                Pose(*model, scene.TryGet<AnimatorComponent>(transform.entity));
                const std::span<const Math::Matrix4f> joints = m_poseEvaluator.GetJointMatrices();
                m_jointMatrices.insert(m_jointMatrices.end(), joints.begin(), joints.end());
            }
            m_draws.push_back({model, viewProjection * world, normalColumns, tint, firstJoint});
        }
        UploadJoints(frameIndex);
    }

    // Pose d'un modèle animé, d'après la lecture de son AnimatorComponent ; sans lui, la pose de repos.
    // Avant le jeu (rien de joué encore), la première image de l'animation choisie.
    void MeshRenderer::Pose(const GpuModel& model, const AnimatorComponent* animator) {
        if (!animator) {
            m_poseEvaluator.Evaluate(model.skeleton, {});
            return;
        }
        const std::string& playing = animator->playing.empty() ? animator->animation : animator->playing;
        const AnimationSample to{FindAnimation(model.skeleton, playing), animator->time, animator->loop && !animator->playingOnce};
        if (animator->fade >= 1.0f) {
            m_poseEvaluator.Evaluate(model.skeleton, to);
            return;
        }
        const AnimationSample from{FindAnimation(model.skeleton, animator->previous), animator->previousTime,
                                   animator->loop && !animator->previousOnce};
        m_poseEvaluator.Evaluate(model.skeleton, from, to, animator->fade);
    }

    // Matrices des os de l'image, par colonnes comme jointColumns dans shaders/Mesh.slang. Si le buffer ne peut
    // grandir (mémoire épuisée), les modèles animés ne sont pas dessinés plutôt que lus hors du buffer.
    void MeshRenderer::UploadJoints(std::uint32_t frameIndex) {
        if (m_jointMatrices.empty()) {
            return;
        }
        static_assert(sizeof(Math::Matrix4f) == 64);
        const std::uint64_t bytes = m_jointMatrices.size() * sizeof(Math::Matrix4f);
        if (GrowJointBuffer(frameIndex, bytes)) {
            m_jointBuffers[frameIndex]->Write(m_jointMatrices.data(), bytes);
        } else {
            std::erase_if(m_draws, [](const MeshDraw& draw) { return !draw.model->skeleton.joints.empty(); });
        }
    }

    void MeshRenderer::Draw(RHI::CommandList& commands, std::uint32_t frameIndex) const {
        // Changements d'état seulement quand il le faut : les entités d'un même modèle se suivent souvent.
        const RHI::Pipeline* boundPipeline = nullptr;
        const GpuModel* boundModel = nullptr;
        const RHI::BindGroup* boundGroup = nullptr;
        bool jointsBound = false;
        for (const MeshDraw& draw : m_draws) {
            const GpuModel& model = *draw.model;
            const bool animated = model.IsAnimated();
            if (&model != boundModel) {
                boundModel = &model;
                const std::array<const RHI::Buffer*, 2> vertexBuffers{model.vertices.get(), model.skin.get()};
                commands.BindVertexBuffers(0, std::span(vertexBuffers).first(animated ? 2 : 1));
                commands.BindIndexBuffer(*model.indices);
            }
            for (const ModelPrimitive& primitive : model.primitives) {
                const GpuMaterial& material = model.materials[primitive.material];
                const RHI::Pipeline& pipeline = animated ? (material.doubleSided ? *m_skinnedDoubleSided : *m_skinned)
                                                         : (material.doubleSided ? *m_staticDoubleSided : *m_static);
                if (&pipeline != boundPipeline) {
                    // Autre pipeline : ses groupes sont reliés, sans compter sur la compatibilité des layouts.
                    boundPipeline = &pipeline;
                    boundGroup = nullptr;
                    jointsBound = false;
                    commands.BindPipeline(pipeline);
                }
                if (animated && !jointsBound) {
                    jointsBound = true;
                    commands.SetBindGroup(1, *m_jointGroups[frameIndex]);
                }
                const RHI::BindGroup* group = material.group ? material.group.get() : &m_models->GetWhiteGroup();
                if (group != boundGroup) {
                    boundGroup = group;
                    commands.SetBindGroup(0, *group);
                }
                MeshParams params{draw.modelViewProjection, draw.normalColumns,
                                  {material.baseColor.r * draw.tint.r, material.baseColor.g * draw.tint.g,
                                   material.baseColor.b * draw.tint.b, material.baseColor.a * draw.tint.a}};
                params.normalColumns[0][3] = material.alphaCutoff;
                params.normalColumns[1][3] = std::bit_cast<float>(draw.firstJoint);
                commands.PushConstants(&params, sizeof(params));
                commands.DrawIndexed(primitive.indexCount, primitive.firstIndex);
            }
        }
    }
}
