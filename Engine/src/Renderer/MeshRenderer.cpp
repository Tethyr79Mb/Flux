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

        // Doit correspondre à FrameData dans shaders/Mesh.slang.
        struct FrameData
        {
            Math::Matrix4f viewProjection;
            std::array<Math::Matrix4f, ShadowCascadeCount> cascades;
            std::array<float, 4> cascadeEnds;
            std::array<float, 4> cascadeTexels;
            std::array<float, 4> cameraForward;
            std::array<float, 4> toSun;
        };
        static_assert(sizeof(FrameData) == 384 && ShadowCascadeCount == 4);

        // Doit correspondre à DrawParams dans shaders/Mesh.slang : envoyé avec chaque primitive.
        struct DrawParams
        {
            Math::LinearColor color;
            std::uint32_t draw;
            std::uint32_t cascade;
            float alphaCutoff;
            std::uint32_t transparent;
        };
        static_assert(sizeof(DrawParams) == 32);

        // Emplacements du groupe 1 (voir shaders/Mesh.slang).
        constexpr std::uint32_t JointSlot = 0;
        constexpr std::uint32_t DrawSlot = 1;
        constexpr std::uint32_t FrameSlot = 2;
        constexpr std::uint32_t ShadowMapSlot = 3;

        // Tailles minimales des buffers de l'image : de quoi animer des dizaines de personnages et dessiner des
        // centaines d'entités sans réallouer.
        constexpr std::uint64_t MinJointBufferSize = 64 * 1024;
        constexpr std::uint64_t MinDrawBufferSize = 64 * 1024;

        // Carte d'ombres : 2 x 2 cascades de 2048 texels de côté (64 Mo en profondeur 32 bits).
        constexpr std::uint32_t ShadowCascadeResolution = 2048;
        constexpr std::uint32_t ShadowMapResolution = 2 * ShadowCascadeResolution;

        // Direction du soleil, fixe dans le monde pour l'instant (la lumière de la scène viendra d'un composant).
        const Math::Vector3f ToSun = Math::Normalize(Math::Vector3f{0.36f, 0.89f, 0.27f});

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

        std::array<float, 4> ToArray(const Math::Vector3f& v) { return {v.x, v.y, v.z, 0.0f}; }
    }

    std::expected<std::unique_ptr<MeshRenderer>, std::string> MeshRenderer::Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                   RHI::Format depthFormat) {
        std::unique_ptr<MeshRenderer> renderer(new MeshRenderer(device));
        auto models = ModelCache::Create(device);
        if (!models) {
            return std::unexpected(models.error());
        }
        renderer->m_models = std::move(*models);

        auto shadowMap = device.CreateRenderTarget(RHI::Format::D32Float, {ShadowMapResolution, ShadowMapResolution}, true);
        if (!shadowMap) {
            return std::unexpected(shadowMap.error());
        }
        renderer->m_shadowMap = std::move(*shadowMap);
        auto shadowSampler = device.CreateSampler({.filter = RHI::Filter::Linear, .mipFilter = RHI::Filter::Nearest,
                                                   .address = RHI::AddressMode::ClampToEdge, .compare = true});
        if (!shadowSampler) {
            return std::unexpected(shadowSampler.error());
        }
        renderer->m_shadowSampler = std::move(*shadowSampler);

        constexpr RHI::ShaderStage both = RHI::ShaderStage::Vertex | RHI::ShaderStage::Fragment;
        constexpr std::array bindings{
            RHI::Binding{JointSlot, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Vertex},
            RHI::Binding{DrawSlot, RHI::BindingType::StorageBuffer, both},
            RHI::Binding{FrameSlot, RHI::BindingType::StorageBuffer, both},
            RHI::Binding{ShadowMapSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
        };
        auto layout = device.CreateBindGroupLayout({.bindings = bindings, .maxGroups = RHI::FramesInFlight});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        renderer->m_frameLayout = std::move(*layout);
        for (std::uint32_t f = 0; f < RHI::FramesInFlight; ++f) {
            renderer->m_frameGroups[f] = device.CreateBindGroup(*renderer->m_frameLayout);
            if (!renderer->m_frameGroups[f]) {
                return std::unexpected("allocation des groupes de l'image");
            }
            if (!renderer->GrowBuffer(renderer->m_jointBuffers[f], f, JointSlot, MinJointBufferSize)
                || !renderer->GrowBuffer(renderer->m_drawBuffers[f], f, DrawSlot, MinDrawBufferSize)
                || !renderer->GrowBuffer(renderer->m_frameBuffers[f], f, FrameSlot, sizeof(FrameData))) {
                return std::unexpected("création des buffers de l'image");
            }
            renderer->m_frameGroups[f]->SetTexture(ShadowMapSlot, *renderer->m_shadowMap, *renderer->m_shadowSampler);
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
        // Ombres : la normale n'est pas lue (profondeur seule) ; la déclarer quand même fait avertir la validation
        // Vulkan (attribut non consommé, lu pour rien par le GPU).
        const std::array shadowAttributes{attributes[0], attributes[2], attributes[3], attributes[4]};
        const std::array<const RHI::BindGroupLayout*, 2> layouts{&m_models->GetMaterialLayout(), m_frameLayout.get()};

        const auto create = [&](std::unique_ptr<RHI::Pipeline>& pipeline, const RHI::PipelineDesc& desc) {
            auto created = m_device.CreatePipeline(desc);
            if (created) {
                pipeline = std::move(*created);
            }
            return created.has_value() ? std::expected<void, std::string>{} : std::unexpected(created.error());
        };
        const auto createAll = [&](Pipelines& pipelines, bool skinned) -> std::expected<void, std::string> {
            RHI::PipelineDesc desc;
            desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Mesh.spv";
            // Modèles animés : os et poids en plus (flux 1).
            desc.vertexBuffers = std::span(vertexBuffers).first(skinned ? 2 : 1);
            desc.vertexAttributes = std::span(attributes).first(skinned ? 5 : 3);
            desc.bindGroupLayouts = layouts;
            desc.pushConstantsSize = sizeof(DrawParams);
            desc.depthTest = true;
            desc.depthWrite = true;

            // Carte d'ombres : profondeur seule, la plus proche du soleil (0 côté soleil). Les deux faces : une
            // coque ouverte ou un matériau double face fait aussi de l'ombre. Le décalage de profondeur (selon la
            // pente) évite qu'une surface s'ombre elle-même par endroits (« acné »), avec le décalage le long de la
            // normale fait à la lecture.
            desc.vertexEntryPoint = skinned ? "vertexShadowSkinned" : "vertexShadow";
            desc.vertexAttributes = std::span(shadowAttributes).first(skinned ? 4 : 2);
            desc.fragmentEntryPoint = "fragmentShadow";
            desc.colorFormat = RHI::Format::Undefined;
            desc.depthFormat = RHI::Format::D32Float;
            desc.depthCompare = RHI::CompareOp::Less;
            desc.cullMode = RHI::CullMode::None;
            desc.depthBiasConstant = 1.0f;
            desc.depthBiasSlope = 1.5f;
            if (auto result = create(pipelines.shadow, desc); !result) {
                return result;
            }

            desc.vertexEntryPoint = skinned ? "vertexSkinned" : "vertexMain";
            desc.vertexAttributes = std::span(attributes).first(skinned ? 5 : 3);
            desc.fragmentEntryPoint = "fragmentMain";
            desc.colorFormat = colorFormat;
            desc.depthFormat = depthFormat;
            desc.depthCompare = RHI::CompareOp::Greater; // reverse-Z
            desc.depthBiasConstant = 0.0f;
            desc.depthBiasSlope = 0.0f;
            const auto createMain = [&](std::unique_ptr<RHI::Pipeline>& pipeline, RHI::CullMode cullMode, bool transparent) {
                desc.cullMode = cullMode;
                // Transparents : mélangés à ce qui est déjà dessiné, cachés par les opaques plus proches, mais sans
                // écrire la profondeur (un transparent ne doit pas masquer ce qui est derrière lui).
                desc.depthWrite = !transparent;
                desc.blend = transparent ? RHI::BlendMode::Alpha : RHI::BlendMode::Opaque;
                return create(pipeline, desc);
            };
            if (auto result = createMain(pipelines.opaque, RHI::CullMode::Back, false); !result) {
                return result;
            }
            if (auto result = createMain(pipelines.opaqueDoubleSided, RHI::CullMode::None, false); !result) {
                return result;
            }
            if (auto result = createMain(pipelines.transparent, RHI::CullMode::Back, true); !result) {
                return result;
            }
            return createMain(pipelines.transparentBack, RHI::CullMode::Front, true);
        };
        if (auto result = createAll(m_static, false); !result) {
            return result;
        }
        return createAll(m_skinned, true);
    }

    // Buffer de l'image f (emplacement `slot` du groupe 1) agrandi à size octets au moins, et son groupe mis à jour.
    // Appelé quand le GPU n'utilise plus cette image (après BeginFrame).
    bool MeshRenderer::GrowBuffer(std::unique_ptr<RHI::Buffer>& buffer, std::uint32_t f, std::uint32_t slot, std::uint64_t size) {
        if (buffer && buffer->GetSize() >= size) {
            return true;
        }
        auto grown = m_device.CreateBuffer({std::bit_ceil(size), RHI::BufferUsage::Storage, RHI::MemoryType::Upload});
        if (!grown) {
            std::fprintf(stderr, "Flux : %s\n", grown.error().c_str());
            return false;
        }
        buffer = std::move(*grown);
        m_frameGroups[f]->SetBuffer(slot, *buffer);
        return true;
    }

    void MeshRenderer::Prepare(const Scene& scene, const Camera& camera, float aspect, Entity hidden,
                               const std::filesystem::path& assetsDirectory, std::uint32_t frameIndex) {
        // La soustraction des positions se fait en double (secteurs compris) : le GPU ne voit que de petits nombres.
        scene.ComputeRelativeTransforms(camera.position, m_relativeTransforms);
        m_draws.clear();
        m_drawData.clear();
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
            if (!model) {
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
            std::uint32_t firstJoint = 0;
            if (!model->skeleton.joints.empty()) {
                firstJoint = static_cast<std::uint32_t>(m_jointMatrices.size());
                Pose(*model, scene.TryGet<AnimatorComponent>(transform.entity));
                const std::span<const Math::Matrix4f> joints = m_poseEvaluator.GetJointMatrices();
                m_jointMatrices.insert(m_jointMatrices.end(), joints.begin(), joints.end());
            }
            m_drawData.push_back({world, {{{x.x, x.y, x.z, 0.0f},
                                           {y.x, y.y, y.z, std::bit_cast<float>(firstJoint)},
                                           {z.x, z.y, z.z, 0.0f}}}});

            // Sphère englobant la boîte du modèle (un modèle animé peut en sortir un peu : ombre coupée au pire).
            const Math::Vector3f center = model->bounds.GetCenter();
            const Math::Vector3f half = model->bounds.GetSize() * 0.5f;
            const Math::Vector3f scaledHalf{half.x * std::abs(transform.scale.x), half.y * std::abs(transform.scale.y),
                                            half.z * std::abs(transform.scale.z)};
            const Math::Vector3f scaledCenter{center.x * transform.scale.x, center.y * transform.scale.y, center.z * transform.scale.z};
            const Math::Vector3f relativeCenter = Math::Vector3f(transform.position) + transform.rotation.Rotate(scaledCenter);
            m_draws.push_back({model, tint, relativeCenter, Math::Length(scaledHalf), Math::Length(relativeCenter),
                               !IsInside(scene, transform.entity, hidden)});
        }
        Upload(camera, aspect, frameIndex);

        // Transparents de l'arrière vers l'avant : chacun se mélange à ce qui est derrière lui. Trié par entité (le
        // centre de sa boîte) : deux transparents qui s'entrecroisent peuvent se superposer dans le mauvais ordre.
        m_transparent.clear();
        for (std::uint32_t d = 0; d < m_draws.size(); ++d) {
            const GpuModel& model = *m_draws[d].model;
            if (!m_draws[d].visible) {
                continue;
            }
            for (std::uint32_t p = 0; p < model.primitives.size(); ++p) {
                if (IsTransparent(m_draws[d], model.materials[model.primitives[p].material])) {
                    m_transparent.push_back({d, p});
                }
            }
        }
        // Stable : les primitives d'une même entité gardent l'ordre du fichier.
        std::stable_sort(m_transparent.begin(), m_transparent.end(), [this](const TransparentDraw& a, const TransparentDraw& b) {
            return m_draws[a.draw].distance > m_draws[b.draw].distance;
        });
    }

    bool MeshRenderer::IsTransparent(const MeshDraw& draw, const GpuMaterial& material) {
        return material.blend || draw.tint.a < 1.0f;
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

    // Envoie au GPU les matrices des os (par colonnes comme jointColumns dans shaders/Mesh.slang), celles des entités
    // et les données de l'image (caméra, cascades). Si un buffer ne peut grandir (mémoire épuisée), rien n'est dessiné
    // plutôt que lu hors du buffer.
    void MeshRenderer::Upload(const Camera& camera, float aspect, std::uint32_t frameIndex) {
        static_assert(sizeof(Math::Matrix4f) == 64 && sizeof(DrawData) == 112);
        const std::uint64_t jointBytes = m_jointMatrices.size() * sizeof(Math::Matrix4f);
        const std::uint64_t drawBytes = m_drawData.size() * sizeof(DrawData);
        if (!GrowBuffer(m_jointBuffers[frameIndex], frameIndex, JointSlot, jointBytes)
            || !GrowBuffer(m_drawBuffers[frameIndex], frameIndex, DrawSlot, drawBytes)) {
            m_draws.clear();
            return;
        }
        if (jointBytes > 0) {
            m_jointBuffers[frameIndex]->Write(m_jointMatrices.data(), jointBytes);
        }
        if (drawBytes > 0) {
            m_drawBuffers[frameIndex]->Write(m_drawData.data(), drawBytes);
        }

        m_cascades = ComputeShadowCascades(camera, aspect, ToSun, ShadowCascadeResolution);
        FrameData frame{};
        frame.viewProjection = camera.GetProjectionMatrix(aspect) * camera.GetViewMatrix();
        for (std::size_t c = 0; c < ShadowCascadeCount; ++c) {
            frame.cascades[c] = m_cascades[c].lightViewProjection;
            frame.cascadeEnds[c] = ShadowCascadeEnds[c];
            frame.cascadeTexels[c] = m_cascades[c].texelSize;
        }
        frame.cameraForward = ToArray(camera.GetForward());
        frame.toSun = ToArray(ToSun);
        m_frameBuffers[frameIndex]->Write(&frame, sizeof(frame));
    }

    // L'entité peut faire de l'ombre dans la cascade : sa sphère touche le volume vu par la carte (le carré couvert,
    // de casterDistance devant la tranche jusqu'à son arrière).
    bool MeshRenderer::InCascade(const MeshDraw& draw, std::size_t cascade) const {
        const ShadowCascade& c = m_cascades[cascade];
        const auto& m = c.lightViewProjection.columns;
        const Math::Vector3f p = draw.center;
        const float x = m[0][0] * p.x + m[1][0] * p.y + m[2][0] * p.z + m[3][0];
        const float y = m[0][1] * p.x + m[1][1] * p.y + m[2][1] * p.z + m[3][1];
        const float z = m[0][2] * p.x + m[1][2] * p.y + m[2][2] * p.z + m[3][2];
        const float side = draw.radius / c.radius;
        const float depth = draw.radius / c.depthRange;
        return std::abs(x) <= 1.0f + side && std::abs(y) <= 1.0f + side && z >= -depth && z <= 1.0f + depth;
    }

    void MeshRenderer::DrawPrimitive(RHI::CommandList& commands, BindState& state, std::uint32_t frameIndex,
                                     std::uint32_t drawIndex, const ModelPrimitive& primitive, const RHI::Pipeline& pipeline,
                                     std::uint32_t cascade) const {
        const MeshDraw& draw = m_draws[drawIndex];
        const GpuModel& model = *draw.model;
        if (&model != state.model) {
            state.model = &model;
            const std::array<const RHI::Buffer*, 2> vertexBuffers{model.vertices.get(), model.skin.get()};
            commands.BindVertexBuffers(0, std::span(vertexBuffers).first(model.IsAnimated() ? 2 : 1));
            commands.BindIndexBuffer(*model.indices);
        }
        if (&pipeline != state.pipeline) {
            // Autre pipeline : ses groupes sont reliés, sans compter sur la compatibilité des layouts.
            state.pipeline = &pipeline;
            state.group = nullptr;
            state.frameBound = false;
            commands.BindPipeline(pipeline);
        }
        if (!state.frameBound) {
            state.frameBound = true;
            commands.SetBindGroup(1, *m_frameGroups[frameIndex]);
        }
        const GpuMaterial& material = model.materials[primitive.material];
        const RHI::BindGroup* group = material.group ? material.group.get() : &m_models->GetWhiteGroup();
        if (group != state.group) {
            state.group = group;
            commands.SetBindGroup(0, *group);
        }
        const DrawParams params{{material.baseColor.r * draw.tint.r, material.baseColor.g * draw.tint.g,
                                 material.baseColor.b * draw.tint.b, material.baseColor.a * draw.tint.a},
                                drawIndex, cascade, material.alphaCutoff, IsTransparent(draw, material) ? 1u : 0u};
        commands.PushConstants(&params, sizeof(params));
        commands.DrawIndexed(primitive.indexCount, primitive.firstIndex);
    }

    void MeshRenderer::DrawShadows(RHI::CommandList& commands, std::uint32_t frameIndex) const {
        commands.BeginRenderPass({.clearColor = {}, .depth = m_shadowMap.get(), .clearDepth = 1.0f, .color = false});
        BindState state;
        for (std::uint32_t c = 0; c < ShadowCascadeCount; ++c) {
            // Cascade c : colonne c % 2, ligne c / 2 de la carte.
            const std::int32_t x = static_cast<std::int32_t>((c % 2) * ShadowCascadeResolution);
            const std::int32_t y = static_cast<std::int32_t>((c / 2) * ShadowCascadeResolution);
            constexpr auto size = static_cast<float>(ShadowCascadeResolution);
            commands.SetViewport({static_cast<float>(x), static_cast<float>(y), size, size});
            commands.SetScissor({x, y, ShadowCascadeResolution, ShadowCascadeResolution});
            for (std::uint32_t d = 0; d < m_draws.size(); ++d) {
                const MeshDraw& draw = m_draws[d];
                if (!InCascade(draw, c)) {
                    continue;
                }
                const RHI::Pipeline& pipeline = *(draw.model->IsAnimated() ? m_skinned : m_static).shadow;
                for (const ModelPrimitive& primitive : draw.model->primitives) {
                    // Les transparents laissent passer la lumière (ni ombre colorée ni ombre partielle pour l'instant).
                    if (!IsTransparent(draw, draw.model->materials[primitive.material])) {
                        DrawPrimitive(commands, state, frameIndex, d, primitive, pipeline, c);
                    }
                }
            }
        }
        commands.EndRenderPass();
    }

    void MeshRenderer::Draw(RHI::CommandList& commands, std::uint32_t frameIndex) const {
        // Changements d'état seulement quand il le faut : les entités d'un même modèle se suivent souvent.
        BindState state;
        for (std::uint32_t d = 0; d < m_draws.size(); ++d) {
            const MeshDraw& draw = m_draws[d];
            if (!draw.visible) {
                continue;
            }
            const Pipelines& pipelines = draw.model->IsAnimated() ? m_skinned : m_static;
            for (const ModelPrimitive& primitive : draw.model->primitives) {
                const GpuMaterial& material = draw.model->materials[primitive.material];
                if (!IsTransparent(draw, material)) {
                    DrawPrimitive(commands, state, frameIndex, d, primitive,
                                  material.doubleSided ? *pipelines.opaqueDoubleSided : *pipelines.opaque);
                }
            }
        }
        for (const TransparentDraw& item : m_transparent) {
            const MeshDraw& draw = m_draws[item.draw];
            const Pipelines& pipelines = draw.model->IsAnimated() ? m_skinned : m_static;
            const ModelPrimitive& primitive = draw.model->primitives[item.primitive];
            // Double face : les faces arrière (le fond de l'objet) d'abord, puis les faces avant par-dessus.
            // Exact pour une forme convexe (bulle de verrière, cube).
            if (draw.model->materials[primitive.material].doubleSided) {
                DrawPrimitive(commands, state, frameIndex, item.draw, primitive, *pipelines.transparentBack);
            }
            DrawPrimitive(commands, state, frameIndex, item.draw, primitive, *pipelines.transparent);
        }
    }
}
