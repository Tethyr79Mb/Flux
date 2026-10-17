#include "Renderer/MeshRenderer.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Graphics/Camera.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>

namespace Flux {
    // Champ de la caméra (à l'origine : positions relatives à la caméra), calculé une fois par image.
    class ViewCone {
        public:
        ViewCone(const Camera& camera, float aspect)
            : m_forward(camera.GetForward()), m_right(camera.GetRight()), m_up(camera.GetUp()),
              m_tanY(std::tan(0.5f * camera.verticalFov)), m_tanX(m_tanY * aspect),
              m_scaleX(1.0f / std::sqrt(1.0f + m_tanX * m_tanX)), m_scaleY(1.0f / std::sqrt(1.0f + m_tanY * m_tanY)) {}

        // La sphère touche le champ : devant la caméra et entre ses quatre bords. Distance signée au plan d'un
        // bord (dehors si positive) : (côté - profondeur x tan) / sqrt(1 + tan²).
        bool Contains(const Math::Vector3f& center, float radius) const {
            const float depth = Math::Dot(center, m_forward);
            if (depth < -radius) {
                return false;
            }
            return (std::abs(Math::Dot(center, m_right)) - depth * m_tanX) * m_scaleX <= radius
                && (std::abs(Math::Dot(center, m_up)) - depth * m_tanY) * m_scaleY <= radius;
        }

        private:
        Math::Vector3f m_forward, m_right, m_up;
        float m_tanY, m_tanX, m_scaleX, m_scaleY;
    };

    namespace {
        static_assert(sizeof(ModelVertex) == 32, "ModelVertex doit correspondre à VertexInput dans shaders/Mesh.slang");
        static_assert(sizeof(ModelSkinVertex) == 24, "ModelSkinVertex doit correspondre à SkinnedVertexInput dans shaders/Mesh.slang");
        static_assert(sizeof(Planet::MorphVertex) == 24, "MorphVertex doit correspondre à PlanetVertexInput dans shaders/Mesh.slang");

        // Volumes intérieurs envoyés au GPU au plus (les plus proches de la caméra) : chaque pixel les parcourt.
        constexpr std::size_t MaxInteriors = 8;

        // Doit correspondre à InteriorData dans shaders/Mesh.slang : une boîte (InteriorComponent).
        struct InteriorData
        {
            // Lignes de la rotation inverse ; w : de quoi passer d'une position relative à la caméra au repère de la
            // boîte (local = dot(axe.xyz, p) + axe.w).
            std::array<std::array<float, 4>, 3> axes;
            std::array<float, 4> halfSize; // w : fondu (m)
            std::array<float, 4> ambient;  // couleur x intensité
        };
        static_assert(sizeof(InteriorData) == 80);

        // Planètes dessinées au plus dans une image (leur sol) : au-delà, les suivantes prennent la première.
        constexpr std::size_t MaxPlanets = 4;

        // Doit correspondre à FrameData dans shaders/Mesh.slang.
        struct FrameData
        {
            Math::Matrix4f viewProjection; // décalée d'un sous-pixel (TemporalView::jitter)
            Math::Matrix4f previousViewProjection; // de l'image précédente, sans décalage
            std::array<Math::Matrix4f, ShadowCascadeCount> cascades;
            std::array<float, 4> cascadeEnds;
            std::array<float, 4> cascadeTexels;
            std::array<float, 4> cameraForward;
            std::array<float, 4> cameraDelta; // TemporalView::cameraDelta
            std::array<float, 4> toSun;
            std::array<float, 4> sunColor;      // couleur x intensité
            // x : multiplie la texture cube du ciel ; y : niveau de mip de la lumière diffuse ; z : niveau de mip des
            // reflets les plus flous ; w : exposition
            std::array<float, 4> environment;
            // Rayon d'un pixel (nuages) : avant + droite x clip.x - haut x clip.y (droite et haut à l'échelle du champ) ;
            // w : décalage d'un sous-pixel (TemporalView::jitter), à retirer du clip du pixel.
            std::array<float, 4> cameraRight;
            std::array<float, 4> cameraUp;
            std::array<std::uint32_t, 4> lightCount; // x : lumières ; y : volumes intérieurs ; z : planètes ; w : TemporalView::noiseFrame
            std::array<InteriorData, MaxInteriors> interiors;
            std::array<PlanetGpu, MaxPlanets> planets;
            AtmosphereGpu atmosphere;
        };
        static_assert(sizeof(FrameData) == 544 + MaxInteriors * sizeof(InteriorData) + MaxPlanets * sizeof(PlanetGpu) + sizeof(AtmosphereGpu)
                      && ShadowCascadeCount == 4);


        // Doit correspondre à DrawParams dans shaders/Mesh.slang : le matériau d'une primitive, envoyé avec chaque lot
        // (ModelBatch). Ce qui est propre à chaque entité (teinte, émissif, métal, rugosité) est dans ses DrawData.
        struct DrawParams
        {
            Math::LinearColor color;
            Math::LinearColor emissive;
            std::uint32_t cascade;
            float alphaCutoff;
            std::uint32_t transparent;
            float metallic;
            float roughness;
            float normalScale;
            float occlusionStrength;
            std::uint32_t padding0;
            std::array<float, 4> padding;
        };
        static_assert(sizeof(DrawParams) == 80);

        // Emplacements du groupe 1 (voir shaders/Mesh.slang).
        constexpr std::uint32_t JointSlot = 0;
        constexpr std::uint32_t DrawSlot = 1;
        constexpr std::uint32_t FrameSlot = 2;
        constexpr std::uint32_t ShadowMapSlot = 3;
        constexpr std::uint32_t LightSlot = 4;
        constexpr std::uint32_t EnvironmentSlot = 5;
        constexpr std::uint32_t InstanceSlot = 6;

        // Tailles minimales des buffers de l'image : de quoi animer des dizaines de personnages et dessiner des
        // centaines d'entités sans réallouer.
        constexpr std::uint64_t MinJointBufferSize = 64 * 1024;
        constexpr std::uint64_t MinDrawBufferSize = 64 * 1024;
        constexpr std::uint64_t MinLightBufferSize = 16 * 1024;
        constexpr std::uint64_t MinInstanceBufferSize = 16 * 1024;
        constexpr std::uint64_t MinModelCommandBufferSize = 16 * 1024;
        // Commandes des tuiles de planète : de quoi dessiner un millier de tuiles dans chaque passe sans réallouer.
        constexpr std::uint64_t MinIndirectBufferSize = 128 * 1024;

        // Lumières envoyées au GPU au plus, les plus proches de la caméra parmi celles qui touchent son champ.
        // Chaque pixel les parcourt toutes : le coût croît avec leur nombre (voir SceneLights dans shaders/Mesh.slang).
        constexpr std::size_t MaxLights = 256;

        // Carte d'ombres : 2 x 2 cascades de 2048 texels de côté (64 Mo en profondeur 32 bits).
        constexpr std::uint32_t ShadowCascadeResolution = 2048;
        constexpr std::uint32_t ShadowMapResolution = 2 * ShadowCascadeResolution;

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

        std::array<float, 4> ToArray(const Math::Vector3f& v, float w = 0.0f) { return {v.x, v.y, v.z, w}; }

        bool InView(const Camera& camera, float aspect, const Math::Vector3f& center, float radius) {
            return ViewCone(camera, aspect).Contains(center, radius);
        }
    }

    std::expected<std::unique_ptr<MeshRenderer>, std::string> MeshRenderer::Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                   RHI::Format motionFormat, RHI::Format depthFormat) {
        std::unique_ptr<MeshRenderer> renderer(new MeshRenderer(device));
        auto models = ModelCache::Create(device);
        if (!models) {
            return std::unexpected(models.error());
        }
        renderer->m_models = std::move(*models);
        auto planets = PlanetRenderer::Create(device);
        if (!planets) {
            return std::unexpected(planets.error());
        }
        renderer->m_planets = std::move(*planets);

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
            RHI::Binding{LightSlot, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Fragment},
            RHI::Binding{EnvironmentSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
            RHI::Binding{InstanceSlot, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Vertex},
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
                || !renderer->GrowBuffer(renderer->m_frameBuffers[f], f, FrameSlot, sizeof(FrameData))
                || !renderer->GrowBuffer(renderer->m_lightBuffers[f], f, LightSlot, MinLightBufferSize)
                || !renderer->GrowBuffer(renderer->m_instanceBuffers[f], f, InstanceSlot, MinInstanceBufferSize)) {
                return std::unexpected("création des buffers de l'image");
            }
            renderer->m_frameGroups[f]->SetTexture(ShadowMapSlot, *renderer->m_shadowMap, *renderer->m_shadowSampler);
        }

        if (auto pipelines = renderer->CreatePipelines(colorFormat, motionFormat, depthFormat); !pipelines) {
            return std::unexpected(pipelines.error());
        }
        return renderer;
    }

    std::expected<void, std::string> MeshRenderer::CreatePipelines(RHI::Format colorFormat, RHI::Format motionFormat, RHI::Format depthFormat) {
        // Flux 0 : ModelVertex pour tous. Flux 1 : os et poids (modèles animés) ou forme parente (tuiles de planète).
        const std::array staticBuffers{RHI::VertexBufferLayout{0, sizeof(ModelVertex)}};
        const std::array skinnedBuffers{RHI::VertexBufferLayout{0, sizeof(ModelVertex)},
                                        RHI::VertexBufferLayout{1, sizeof(ModelSkinVertex)}};
        const std::array planetBuffers{RHI::VertexBufferLayout{0, sizeof(ModelVertex)},
                                       RHI::VertexBufferLayout{1, sizeof(Planet::MorphVertex)}};
        const RHI::VertexAttribute position{0, 0, RHI::Format::RGB32Float, offsetof(ModelVertex, position)};
        const RHI::VertexAttribute normal{1, 0, RHI::Format::RGB32Float, offsetof(ModelVertex, normal)};
        const RHI::VertexAttribute uv{2, 0, RHI::Format::RG32Float, offsetof(ModelVertex, uv)};
        const RHI::VertexAttribute joints{3, 1, RHI::Format::RGBA16Uint, offsetof(ModelSkinVertex, joints)};
        const RHI::VertexAttribute weights{4, 1, RHI::Format::RGBA32Float, offsetof(ModelSkinVertex, weights)};
        const RHI::VertexAttribute morphOffset{5, 1, RHI::Format::RGB32Float, offsetof(Planet::MorphVertex, offset)};
        const RHI::VertexAttribute morphNormal{6, 1, RHI::Format::RGB32Float, offsetof(Planet::MorphVertex, normal)};
        // Ombres : les normales ne sont pas lues (profondeur seule) ; les déclarer quand même fait avertir la
        // validation Vulkan (attribut non consommé, lu pour rien par le GPU).
        const std::array staticAttributes{position, normal, uv};
        const std::array staticShadow{position, uv};
        const std::array skinnedAttributes{position, normal, uv, joints, weights};
        const std::array skinnedShadow{position, uv, joints, weights};
        const std::array planetAttributes{position, normal, uv, morphOffset, morphNormal};
        const std::array planetShadow{position, uv, morphOffset};
        const std::array oceanAttributes{position, morphOffset}; // l'eau fait sa normale et sa profondeur elle-même
        const std::array<const RHI::BindGroupLayout*, 2> layouts{&m_models->GetMaterialLayout(), m_frameLayout.get()};

        struct Kind
        {
            std::span<const RHI::VertexBufferLayout> buffers;
            std::span<const RHI::VertexAttribute> attributes;
            std::span<const RHI::VertexAttribute> shadowAttributes;
            const char* vertex;
            const char* shadowVertex;
            const char* fragment = "fragmentOpaque";
            bool motion = true; // le fragment écrit le mouvement propre (SV_Target1)
            // Modèles : variantes double face et à seuil d'alpha (le sol et l'eau des planètes n'en ont pas).
            bool materials = false;
        };
        const auto create = [&](std::unique_ptr<RHI::Pipeline>& pipeline, const RHI::PipelineDesc& desc) {
            auto created = m_device.CreatePipeline(desc);
            if (created) {
                pipeline = std::move(*created);
            }
            return created.has_value() ? std::expected<void, std::string>{} : std::unexpected(created.error());
        };
        const auto createAll = [&](Pipelines& pipelines, const Kind& kind) -> std::expected<void, std::string> {
            RHI::PipelineDesc desc;
            desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Mesh.spv";
            desc.vertexBuffers = kind.buffers;
            desc.bindGroupLayouts = layouts;
            desc.pushConstantsSize = sizeof(DrawParams);
            desc.depthTest = true;
            desc.depthWrite = true;

            // Carte d'ombres : profondeur seule, la plus proche du soleil (0 côté soleil). Le décalage de profondeur
            // (selon la pente) évite qu'une surface s'ombre elle-même par endroits (« acné »), avec le décalage le
            // long de la normale fait à la lecture. Sans programme de pixel : rien à calculer par texel, sauf pour le
            // seuil d'alpha (feuillage), qui doit lire son image.
            desc.vertexEntryPoint = kind.shadowVertex;
            desc.vertexAttributes = kind.shadowAttributes;
            desc.fragmentEntryPoint = nullptr;
            desc.colorFormat = RHI::Format::Undefined;
            desc.depthFormat = RHI::Format::D32Float;
            desc.depthCompare = RHI::CompareOp::Less;
            desc.depthBiasConstant = 1.0f;
            desc.depthBiasSlope = 1.5f;
            // Les deux faces : une coque ouverte ou un matériau double face fait aussi de l'ombre.
            desc.cullMode = RHI::CullMode::None;
            if (auto result = create(pipelines.shadowDoubleSided, desc); !result) {
                return result;
            }
            // Objet fermé, une face : seules celles tournées vers le soleil, les plus proches de lui, comptent. Vues
            // du soleil, elles tournent dans le sens inverse de celles vues de la caméra (profondeur vers le soleil
            // en 0, au lieu de 1 vers la caméra) : on élimine donc les faces « avant » de la convention du rendu.
            desc.cullMode = RHI::CullMode::Front;
            if (auto result = create(pipelines.shadow, desc); !result) {
                return result;
            }
            if (kind.materials) {
                desc.fragmentEntryPoint = "fragmentShadow";
                desc.cullMode = RHI::CullMode::None;
                if (auto result = create(pipelines.shadowMasked, desc); !result) {
                    return result;
                }
            }

            desc.vertexEntryPoint = kind.vertex;
            desc.vertexAttributes = kind.attributes;
            desc.fragmentEntryPoint = kind.fragment;
            desc.colorFormat = colorFormat;
            desc.secondColorFormat = motionFormat;
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
                // Transparents : le mouvement reste celui de ce qu'on voit à travers.
                desc.writeSecondColor = kind.motion && !transparent;
                return create(pipeline, desc);
            };
            if (auto result = createMain(pipelines.opaque, RHI::CullMode::Back, false); !result) {
                return result;
            }
            if (!kind.materials) {
                return createMain(pipelines.transparent, RHI::CullMode::Back, true);
            }
            if (auto result = createMain(pipelines.opaqueDoubleSided, RHI::CullMode::None, false); !result) {
                return result;
            }
            // Seuil d'alpha et transparents : le programme qui peut jeter des pixels.
            desc.fragmentEntryPoint = "fragmentMain";
            if (auto result = createMain(pipelines.masked, RHI::CullMode::Back, false); !result) {
                return result;
            }
            if (auto result = createMain(pipelines.maskedDoubleSided, RHI::CullMode::None, false); !result) {
                return result;
            }
            if (auto result = createMain(pipelines.transparent, RHI::CullMode::Back, true); !result) {
                return result;
            }
            return createMain(pipelines.transparentBack, RHI::CullMode::Front, true);
        };
        if (auto result = createAll(m_static, {staticBuffers, staticAttributes, staticShadow, "vertexMain", "vertexShadow",
                                               "fragmentOpaque", true, true});
            !result) {
            return result;
        }
        if (auto result = createAll(m_skinned, {skinnedBuffers, skinnedAttributes, skinnedShadow, "vertexSkinned", "vertexShadowSkinned",
                                                "fragmentOpaque", true, true});
            !result) {
            return result;
        }
        if (auto result = createAll(m_planet, {planetBuffers, planetAttributes, planetShadow, "vertexPlanet", "vertexShadowPlanet",
                                               "fragmentPlanet"});
            !result) {
            return result;
        }
        if (auto result = createAll(m_ocean, {planetBuffers, oceanAttributes, planetShadow, "vertexOcean", "vertexShadowPlanet",
                                              "fragmentOcean", false});
            !result) {
            return result;
        }
        // Nuages : la profondeur du point touché est écrite par le shader (SV_Depth) et testée contre celle de la
        // scène, sans être enregistrée : le sol, les montagnes et les vaisseaux devant eux les cachent.
        RHI::PipelineDesc clouds;
        clouds.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Mesh.spv";
        clouds.vertexEntryPoint = "vertexClouds";
        clouds.fragmentEntryPoint = "fragmentClouds";
        clouds.bindGroupLayouts = layouts;
        clouds.pushConstantsSize = sizeof(DrawParams);
        clouds.colorFormat = colorFormat;
        clouds.secondColorFormat = motionFormat;
        clouds.depthFormat = depthFormat;
        clouds.depthTest = true;
        clouds.depthWrite = false;
        clouds.depthCompare = RHI::CompareOp::GreaterOrEqual;
        clouds.blend = RHI::BlendMode::Alpha;
        return create(m_clouds, clouds);
    }

    const RHI::Pipeline& MeshRenderer::ShadowPipeline(const Pipelines& pipelines, const GpuMaterial& material) {
        return material.alphaCutoff > 0.0f ? *pipelines.shadowMasked
             : material.doubleSided        ? *pipelines.shadowDoubleSided
                                           : *pipelines.shadow;
    }

    const RHI::Pipeline& MeshRenderer::OpaquePipeline(const Pipelines& pipelines, const GpuMaterial& material) {
        if (material.alphaCutoff > 0.0f) {
            return material.doubleSided ? *pipelines.maskedDoubleSided : *pipelines.masked;
        }
        return material.doubleSided ? *pipelines.opaqueDoubleSided : *pipelines.opaque;
    }

    const MeshRenderer::Pipelines& MeshRenderer::GetPipelines(const MeshDraw& draw) const {
        return draw.model->IsAnimated() ? m_skinned : m_static;
    }

    // Buffer de l'image f (emplacement `slot` du groupe 1) agrandi à size octets au moins, et son groupe mis à jour.
    // Appelé quand le GPU n'utilise plus cette image (après BeginFrame).
    bool MeshRenderer::GrowBuffer(std::unique_ptr<RHI::Buffer>& buffer, std::uint32_t f, std::uint32_t slot, std::uint64_t size) {
        if (buffer && buffer->GetSize() >= size) {
            return true;
        }
        auto grown = m_device.CreateBuffer({std::bit_ceil(size), RHI::BufferUsage::Storage, RHI::MemoryType::Upload});
        if (!grown) {
            Log::Error("{}", grown.error());
            return false;
        }
        buffer = std::move(*grown);
        m_frameGroups[f]->SetBuffer(slot, *buffer);
        return true;
    }

    void MeshRenderer::Prepare(const Scene& scene, const Camera& camera, float aspect, Entity hidden, const SkyLighting& sky,
                               const TemporalView& temporal, const std::filesystem::path& assetsDirectory,
                               std::uint32_t frameIndex) {
        if (sky.environment && sky.environment != m_boundEnvironment[frameIndex]) {
            m_frameGroups[frameIndex]->SetTexture(EnvironmentSlot, *sky.environment, *sky.environmentSampler);
            m_boundEnvironment[frameIndex] = sky.environment;
        }
        // La soustraction des positions se fait en double (secteurs compris) : le GPU ne voit que de petits nombres.
        scene.ComputeRelativeTransforms(camera.position, m_relativeTransforms);
        ClearDraws();
        m_drawData.clear();
        m_tileData.clear();
        m_jointMatrices.clear();
        m_lights.clear();
        m_interiors.clear();
        m_thrusters.clear();
        m_planetData.clear();
        m_hasClouds = false;
        ++m_preparedFrame;
        // Textures du sol : lues (sur un autre fil) seulement quand la scène a une planète.
        const bool planets = std::ranges::any_of(m_relativeTransforms, [&scene](const RelativeTransform& transform) {
            return scene.TryGet<PlanetComponent>(transform.entity) != nullptr;
        });
        m_groundGroup = planets ? m_ground.Get(m_device, *m_models, assetsDirectory) : nullptr;
        const ViewCone view(camera, aspect);
        // Avant les entités : celles qui ne sont ni en vue ni dans une ombre sont écartées dès maintenant.
        m_cascades = ComputeShadowCascades(camera, aspect, sky.toSun, ShadowCascadeResolution);
        m_planets->BeginFrame(frameIndex);
        for (const RelativeTransform& transform : m_relativeTransforms) {
            if (const auto* planet = scene.TryGet<PlanetComponent>(transform.entity)) {
                AddPlanet(transform, *planet, scene.TryGet<RimLightComponent>(transform.entity),
                          scene.TryGet<CloudsComponent>(transform.entity), view);
            }
            if (const auto* light = scene.TryGet<LightComponent>(transform.entity)) {
                AddLight(camera, aspect, transform, *light);
            }
            if (const auto* thruster = scene.TryGet<ThrusterComponent>(transform.entity)) {
                AddThruster(scene, camera, aspect, transform, *thruster);
            }
            if (const auto* interior = scene.TryGet<InteriorComponent>(transform.entity)) {
                // Distance de la caméra (l'origine) à la boîte : 0 dedans.
                const Math::Vector3f center(transform.position);
                const Math::Vector3f local = transform.rotation.Conjugate().Rotate(-center);
                const auto outside = [&](float value, float size, float scale) {
                    return std::max(std::abs(value) - 0.5f * size * std::abs(scale), 0.0f);
                };
                const Math::Vector3f gap{outside(local.x, interior->size.x, transform.scale.x),
                                         outside(local.y, interior->size.y, transform.scale.y),
                                         outside(local.z, interior->size.z, transform.scale.z)};
                m_interiors.push_back({transform, interior, Math::Length(gap)});
            }
            const GpuModel* model = nullptr;
            Math::LinearColor tint;
            Math::LinearColor emissive{0.0f, 0.0f, 0.0f};
            float metallic = 1.0f;
            float roughness = 1.0f;
            bool materialOverride = false;
            if (const auto* renderer = scene.TryGet<MeshRendererComponent>(transform.entity)) {
                model = &m_models->GetCube();
                tint = renderer->color;
                emissive = {tint.r * renderer->emission, tint.g * renderer->emission, tint.b * renderer->emission};
                metallic = std::clamp(renderer->metallic, 0.0f, 1.0f);
                roughness = std::clamp(renderer->roughness, 0.0f, 1.0f);
            } else if (const auto* component = scene.TryGet<ModelComponent>(transform.entity);
                       component && !component->model.empty() && !assetsDirectory.empty()) {
                model = m_models->Find(assetsDirectory, component->model);
                tint = component->tint;
                if (component->overrideMaterial) {
                    materialOverride = true;
                    metallic = std::clamp(component->metallic, 0.0f, 1.0f);
                    roughness = std::clamp(component->roughness, 0.0f, 1.0f);
                }
            }
            if (!model) {
                continue;
            }
            // Sphère englobant la boîte du modèle (un modèle animé peut en sortir un peu : ombre coupée au pire).
            const Math::Vector3f center = model->bounds.GetCenter();
            const Math::Vector3f half = model->bounds.GetSize() * 0.5f;
            const Math::Vector3f scaledHalf{half.x * std::abs(transform.scale.x), half.y * std::abs(transform.scale.y),
                                            half.z * std::abs(transform.scale.z)};
            const Math::Vector3f scaledCenter{center.x * transform.scale.x, center.y * transform.scale.y, center.z * transform.scale.z};
            const Math::Vector3f relativeCenter = Math::Vector3f(transform.position) + transform.rotation.Rotate(scaledCenter);
            const float radius = Math::Length(scaledHalf);
            // Un modèle animé peut sortir de sa boîte de repos (bras levés) : marge pour ne pas l'écarter trop tôt.
            const bool inView = view.Contains(relativeCenter, model->IsAnimated() ? 1.5f * radius : radius);
            // Ni en vue ni dans une ombre : rien à dessiner, ni pose à calculer (personnage loin derrière la caméra).
            if (!inView && !CastsAnyShadow(relativeCenter, radius)) {
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
            // Lumière de contour : celle de l'entité ou du plus proche parent qui en a une (tout un vaisseau).
            std::array<float, 4> rim{};
            for (Entity owner = transform.entity; owner != NullEntity; owner = scene.GetParent(owner)) {
                if (const auto* component = scene.TryGet<RimLightComponent>(owner)) {
                    if (component->intensity > 0.0f) {
                        rim = {component->color.r * component->intensity, component->color.g * component->intensity,
                               component->color.b * component->intensity, std::max(component->sharpness, 0.5f)};
                    }
                    break;
                }
            }
            // Où était l'entité à l'image précédente, vue de l'ancienne caméra : sa matrice d'alors si elle était déjà
            // là, sinon la même place dans le monde (immobile).
            const auto index = static_cast<std::size_t>(entt::to_entity(transform.entity));
            if (index >= m_previousWorld.size()) {
                m_previousWorld.resize(index + 1);
                m_previousFrame.resize(index + 1, 0);
            }
            const bool moved = temporal.history && m_previousFrame[index] + 1 == m_preparedFrame;
            const Math::Matrix4f previous = moved ? m_previousWorld[index] : Math::Matrix4f::Translation(temporal.cameraDelta) * world;
            m_previousWorld[index] = world;
            m_previousFrame[index] = m_preparedFrame;
            m_drawData.push_back({world,
                                  {{{x.x, x.y, x.z, 0.0f}, {y.x, y.y, y.z, std::bit_cast<float>(firstJoint)}, {z.x, z.y, z.z, 0.0f}}},
                                  rim, {}, previous,
                                  {tint.r, tint.g, tint.b, tint.a},
                                  {emissive.r, emissive.g, emissive.b, materialOverride ? 1.0f : 0.0f},
                                  {metallic, roughness, 0.0f, 0.0f}});

            m_draws.push_back({model, tint, emissive, metallic, roughness, materialOverride, relativeCenter, radius,
                               Math::Length(relativeCenter), !IsInside(scene, transform.entity, hidden), inView});
        }
        // Tuiles manquantes des planètes (calculées au début de l'image suivante, dessinées à partir d'elle).
        m_planets->EndFrame(frameIndex);
        if (!m_boundEnvironment[frameIndex]) {
            ClearDraws(); // pas de texture du ciel (mémoire épuisée) : rien plutôt qu'une lecture invalide
        }
        Upload(camera, aspect, sky, temporal, frameIndex);
        if (!BuildTileCommands(frameIndex)) {
            m_tileDraws.clear();
            m_tilePasses = {};
        }

        // Transparents de l'arrière vers l'avant : chacun se mélange à ce qui est derrière lui. Trié par entité (le
        // centre de sa boîte) : deux transparents qui s'entrecroisent peuvent se superposer dans le mauvais ordre.
        // Opaques : rangés en lots (BuildModelBatches). Hors champ : ombre seulement.
        m_transparent.clear();
        m_opaque.clear();
        for (std::uint32_t d = 0; d < m_draws.size(); ++d) {
            const GpuModel& model = *m_draws[d].model;
            if (!m_draws[d].visible || !m_draws[d].inView) {
                continue;
            }
            bool opaque = false;
            for (std::uint32_t p = 0; p < model.primitives.size(); ++p) {
                if (IsTransparent(m_draws[d], model.materials[model.primitives[p].material])) {
                    m_transparent.push_back({d, p});
                } else {
                    opaque = true;
                }
            }
            if (opaque) {
                m_opaque.push_back(d);
            }
        }
        // Stable : les primitives d'une même entité gardent l'ordre du fichier.
        std::stable_sort(m_transparent.begin(), m_transparent.end(), [this](const TransparentDraw& a, const TransparentDraw& b) {
            return m_draws[a.draw].distance > m_draws[b.draw].distance;
        });
        if (!BuildModelBatches(frameIndex)) {
            m_modelPasses = {};
        }
    }

    // Tuiles de la planète à dessiner, relatives à la caméra : leur centre est calculé en double (PlanetRenderer), le
    // GPU ne voit que des sommets proches de ce centre.
    void MeshRenderer::AddPlanet(const RelativeTransform& transform, const PlanetComponent& planet, const RimLightComponent* rim,
                                 const CloudsComponent* clouds, const ViewCone& view) {
        m_planetTiles.clear();
        m_planets->Collect(transform.entity, planet, transform.position, transform.rotation,
                           [&view](const Math::Vector3f& center, float radius) { return view.Contains(center, radius); },
                           m_planetTiles);
        const Math::Matrix4f rotation = transform.rotation.ToMatrix();
        const Math::Vector3f x = transform.rotation.Rotate(Math::Vector3f{1.0f, 0.0f, 0.0f});
        const Math::Vector3f y = transform.rotation.Rotate(Math::Vector3f{0.0f, 1.0f, 0.0f});
        const Math::Vector3f z = transform.rotation.Rotate(Math::Vector3f{0.0f, 0.0f, 1.0f});
        if (m_planetTiles.empty()) {
            return;
        }
        // Son sol, lu par fragmentPlanet ; sans zones, roche partout.
        const float roughness = std::clamp(planet.roughness, 0.0f, 1.0f);
        // Avec les textures du sol, la teinte de chaque matière à la place de sa couleur.
        const auto material = [&](const Math::LinearColor& color, const Math::LinearColor& tint, float materialRoughness) {
            if (m_groundGroup) {
                return std::array{tint.r, tint.g, tint.b, materialRoughness};
            }
            return planet.zones ? std::array{color.r, color.g, color.b, materialRoughness}
                                : std::array{planet.color.r, planet.color.g, planet.color.b, roughness};
        };
        const std::uint32_t index = static_cast<std::uint32_t>(std::min(m_planetData.size(), MaxPlanets - 1));
        const bool added = m_planetData.size() < MaxPlanets;
        if (added) {
            m_planetData.push_back({{std::max(planet.radius, 1.0f), planet.relief, planet.snowLevel * 0.01f * planet.relief, 0.0f},
                                    material(planet.color, planet.rockTint, roughness),
                                    material(planet.grassColor, planet.grassTint, 0.95f),
                                    material(planet.sandColor, planet.sandTint, 0.9f),
                                    material(planet.snowColor, planet.snowTint, 0.55f),
                                    {planet.waterColor.r, planet.waterColor.g, planet.waterColor.b, 0.0f},
                                    material(planet.soilColor, planet.soilTint, 0.95f),
                                    {planet.temperature, planet.poleTemperature, std::clamp(planet.humidity, 0.0f, 1.0f),
                                     m_groundGroup ? 1.0f : 0.0f},
                                    {planet.seaLevel * 0.01f * planet.relief, std::max(planet.beachHeight, 0.0f), 0.0f, 0.0f}});
            if (rim && rim->intensity > 0.0f) {
                const float intensity = rim->intensity;
                m_planetData[index].rim = {rim->color.r * intensity, rim->color.g * intensity, rim->color.b * intensity,
                                           std::max(rim->sharpness, 0.5f)};
            }
            if (clouds) {
                AddClouds(m_planetData[index], transform, planet, *clouds);
                m_hasClouds = m_hasClouds || m_planetData[index].clouds[3] > 0.0f;
            }
        }
        // Surface de l'océan : sur les tuiles qui descendent sous le niveau de la mer.
        const double seaRadius = std::max(planet.radius, 1.0f) + planet.seaLevel * 0.01 * planet.relief;
        const bool ocean = added && planet.ocean;
        if (ocean) {
            m_planetData[index].water[3] = static_cast<float>(seaRadius);
        }
        for (const PlanetRenderer::Tile& tile : m_planetTiles) {
            const Math::Vector3f center(tile.center);
            // Centre de la tuile dans le repère de la planète : où fragmentPlanet lit l'altitude et le bruit.
            const Math::Vector3f local(tile.local);
            // Le même modulo GroundPeriod, calculé en double : les textures du sol restent nettes et sans couture
            // d'une tuile à l'autre, même à des centaines de kilomètres du centre.
            const auto wrap = [](double value) { return static_cast<float>(value - GroundPeriod * std::floor(value / GroundPeriod)); };
            // Eau : mêmes données (vertexOcean pose la tuile sur la sphère de la mer) ; sa sphère englobante grandit de ce
            // que l'eau dépasse du fond.
            // Champ, cascades et ordre de près à loin : par la sphère englobante, pas par le point de référence.
            const Math::Vector3f sphere(tile.sphere);
            const bool water = ocean && tile.lowest < seaRadius
                            && view.Contains(sphere, tile.radius + static_cast<float>(seaRadius - tile.lowest));
            m_tileDraws.push_back({tile.tile, static_cast<std::uint32_t>(m_tileData.size()), sphere, tile.radius,
                                   Math::Length(sphere) - tile.radius, water});
            m_tileData.push_back({Math::Matrix4f::Translation(center) * rotation,
                                  {{{x.x, x.y, x.z, tile.morphStart}, {y.x, y.y, y.z, 0.0f}, {z.x, z.y, z.z, tile.morphScale}}},
                                  {local.x, local.y, local.z, std::bit_cast<float>(index)},
                                  {wrap(tile.local.x), wrap(tile.local.y), wrap(tile.local.z), 0.0f},
                                  {}, {}, {}, {}});
        }
    }

    // Lumière de l'entité, si sa portée touche le champ de la caméra. Un spot éclaire vers le -z de l'entité.
    void MeshRenderer::AddLight(const Camera& camera, float aspect, const RelativeTransform& transform, const LightComponent& light) {
        const Math::Vector3f position(transform.position);
        if (light.range <= 0.0f || light.intensity <= 0.0f || !InView(camera, aspect, position, light.range)) {
            return;
        }
        const Math::Vector3f color{light.color.r * light.intensity, light.color.g * light.intensity, light.color.b * light.intensity};
        LightData data{ToArray(position, light.range), ToArray(color, light.type == LightType::Spot ? 1.0f : 0.0f), {}, {}};
        if (light.type == LightType::Spot) {
            constexpr float RadiansPerDegree = std::numbers::pi_v<float> / 180.0f;
            const float half = 0.5f * std::clamp(light.angle, 1.0f, 179.0f) * RadiansPerDegree;
            const float outer = std::cos(half);
            const float inner = std::cos(half * (1.0f - std::clamp(light.softness, 0.0f, 1.0f)));
            data.direction = ToArray(Math::Normalize(transform.rotation.Rotate(Math::Vector3f{0.0f, 0.0f, -1.0f})), outer);
            data.spot = {1.0f / std::max(inner - outer, 1.0e-4f), 0.0f, 0.0f, 0.0f};
        }
        m_lights.push_back({data, Math::Length(position)});
    }

    // Réacteur : sa flamme (ThrusterRenderer) et la lueur qu'elle jette, une lumière ponctuelle au premier quart de
    // la flamme, d'autant plus forte et plus loin que la poussée est grande. Sur un vaisseau, la poussée de ses
    // moteurs ; sans vaisseau, ou dans l'éditeur jeu arrêté (SetThrusterPreview), toujours à fond.
    void MeshRenderer::AddThruster(const Scene& scene, const Camera& camera, float aspect, const RelativeTransform& transform,
                                   const ThrusterComponent& thruster) {
        float throttle = 1.0f;
        if (const Entity ship = scene.GetShip(transform.entity); ship != NullEntity && !m_thrusterPreview) {
            throttle = std::clamp(scene.Get<ShipComponent>(ship).throttle, 0.0f, 1.0f);
        }
        const float idle = std::clamp(thruster.idle, 0.0f, 1.0f);
        const float shown = idle + (1.0f - idle) * throttle;
        if (shown <= 0.0f) {
            return;
        }
        m_thrusters.push_back({transform, &thruster, shown});
        if (thruster.light <= 0.0f) {
            return;
        }
        const Math::Vector3f box = ThrusterRenderer::GetBox(thruster, shown);
        RelativeTransform glow = transform;
        glow.position += Math::Vector3d(transform.rotation.Rotate(Math::Vector3f{0.0f, 0.0f, 0.25f * box.z * transform.scale.z}));
        const LightComponent light{.type = LightType::Point,
                                   .color = thruster.color,
                                   .intensity = thruster.light * shown,
                                   .range = 2.0f * std::max(box.x, box.y) + 2.0f * box.z};
        AddLight(camera, aspect, glow, light);
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
    void MeshRenderer::Upload(const Camera& camera, float aspect, const SkyLighting& sky, const TemporalView& temporal,
                              std::uint32_t frameIndex) {
        static_assert(sizeof(Math::Matrix4f) == 64 && sizeof(DrawData) == 256 && sizeof(LightData) == 64);
        const std::uint64_t jointBytes = m_jointMatrices.size() * sizeof(Math::Matrix4f);
        // Celles des tuiles des planètes après celles des entités.
        const std::uint64_t drawBytes = m_drawData.size() * sizeof(DrawData);
        const std::uint64_t tileBytes = m_tileData.size() * sizeof(DrawData);
        if (!GrowBuffer(m_jointBuffers[frameIndex], frameIndex, JointSlot, jointBytes)
            || !GrowBuffer(m_drawBuffers[frameIndex], frameIndex, DrawSlot, drawBytes + tileBytes)) {
            ClearDraws();
            return;
        }
        if (jointBytes > 0) {
            m_jointBuffers[frameIndex]->Write(m_jointMatrices.data(), jointBytes);
        }
        if (drawBytes > 0) {
            m_drawBuffers[frameIndex]->Write(m_drawData.data(), drawBytes);
        }
        if (tileBytes > 0) {
            m_drawBuffers[frameIndex]->Write(m_tileData.data(), tileBytes, drawBytes);
        }
        // Trop de lumières : les plus proches de la caméra.
        if (m_lights.size() > MaxLights) {
            std::ranges::nth_element(m_lights, m_lights.begin() + MaxLights, {}, &PreparedLight::distance);
            m_lights.resize(MaxLights);
        }
        m_lightData.clear();
        for (const PreparedLight& light : m_lights) {
            m_lightData.push_back(light.data);
        }
        const std::uint64_t lightBytes = m_lightData.size() * sizeof(LightData);
        if (!GrowBuffer(m_lightBuffers[frameIndex], frameIndex, LightSlot, lightBytes)) {
            m_lightData.clear(); // pas de place : aucune lumière plutôt qu'une lecture hors du buffer
        } else if (lightBytes > 0) {
            m_lightBuffers[frameIndex]->Write(m_lightData.data(), lightBytes);
        }

        FrameData frame{};
        const Math::Matrix4f viewProjection = camera.GetProjectionMatrix(aspect) * camera.GetViewMatrix();
        frame.viewProjection = Jittered(viewProjection, temporal.jitter);
        frame.previousViewProjection = temporal.history ? temporal.previousViewProjection : viewProjection;
        frame.cameraDelta = ToArray(temporal.history ? temporal.cameraDelta : Math::Vector3f{});
        for (std::size_t c = 0; c < ShadowCascadeCount; ++c) {
            frame.cascades[c] = m_cascades[c].lightViewProjection;
            frame.cascadeEnds[c] = ShadowCascadeEnds[c];
            frame.cascadeTexels[c] = m_cascades[c].texelSize;
        }
        // w : secondes écoulées (vagues des océans), repartent de 0 toutes les heures pour garder leur précision.
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
        frame.cameraForward = ToArray(camera.GetForward(), static_cast<float>(std::fmod(seconds, 3600.0)));
        frame.toSun = ToArray(sky.toSun);
        frame.sunColor = ToArray(sky.sunColor);
        // Lumière diffuse : la texture réduite à 4 x 4 par face, une moyenne de tout un côté du ciel. Reflets : de
        // net (rugosité 0) à ce même flou (rugosité 1).
        const float diffuseMip = static_cast<float>(sky.environmentMips > 3 ? sky.environmentMips - 3 : 0);
        frame.environment = {sky.ambient, diffuseMip, diffuseMip, sky.exposure};
        const float tanY = std::tan(0.5f * camera.verticalFov);
        frame.cameraRight = ToArray(camera.GetRight() * (tanY * aspect), temporal.jitter.x);
        frame.cameraUp = ToArray(camera.GetUp() * tanY, temporal.jitter.y);
        // Trop de volumes intérieurs : les plus proches de la caméra (ceux qu'on voit de près).
        if (m_interiors.size() > MaxInteriors) {
            std::ranges::nth_element(m_interiors, m_interiors.begin() + MaxInteriors, {}, &PreparedInterior::distance);
            m_interiors.resize(MaxInteriors);
        }
        for (std::size_t i = 0; i < m_interiors.size(); ++i) {
            const RelativeTransform& transform = m_interiors[i].transform;
            const InteriorComponent& interior = *m_interiors[i].interior;
            const Math::Vector3f center(transform.position);
            InteriorData& data = frame.interiors[i];
            const std::array axes{Math::Vector3f{1.0f, 0.0f, 0.0f}, Math::Vector3f{0.0f, 1.0f, 0.0f}, Math::Vector3f{0.0f, 0.0f, 1.0f}};
            for (std::size_t a = 0; a < 3; ++a) {
                const Math::Vector3f axis = transform.rotation.Rotate(axes[a]);
                data.axes[a] = ToArray(axis, -Math::Dot(axis, center));
            }
            data.halfSize = {0.5f * std::max(interior.size.x, 0.0f) * std::abs(transform.scale.x),
                             0.5f * std::max(interior.size.y, 0.0f) * std::abs(transform.scale.y),
                             0.5f * std::max(interior.size.z, 0.0f) * std::abs(transform.scale.z), std::max(interior.fade, 1.0e-3f)};
            const float intensity = std::max(interior.intensity, 0.0f);
            data.ambient = {interior.ambient.r * intensity, interior.ambient.g * intensity, interior.ambient.b * intensity, 0.0f};
        }
        frame.lightCount = {static_cast<std::uint32_t>(m_lightData.size()), static_cast<std::uint32_t>(m_interiors.size()),
                            static_cast<std::uint32_t>(m_planetData.size()), temporal.noiseFrame};
        std::ranges::copy(m_planetData, frame.planets.begin());
        frame.atmosphere = sky.atmosphere;
        m_frameBuffers[frameIndex]->Write(&frame, sizeof(frame));
    }

    // L'entité peut faire de l'ombre dans la cascade : sa sphère touche le volume vu par la carte (le carré couvert,
    // de casterDistance devant la tranche jusqu'à son arrière).
    bool MeshRenderer::InCascade(const Math::Vector3f& center, float radius, std::size_t cascade) const {
        const ShadowCascade& c = m_cascades[cascade];
        const auto& m = c.lightViewProjection.columns;
        const Math::Vector3f p = center;
        const float x = m[0][0] * p.x + m[1][0] * p.y + m[2][0] * p.z + m[3][0];
        const float y = m[0][1] * p.x + m[1][1] * p.y + m[2][1] * p.z + m[3][1];
        const float z = m[0][2] * p.x + m[1][2] * p.y + m[2][2] * p.z + m[3][2];
        const float side = radius / c.radius;
        const float depth = radius / c.depthRange;
        return std::abs(x) <= 1.0f + side && std::abs(y) <= 1.0f + side && z >= -depth && z <= 1.0f + depth;
    }

    // L'entité fait une ombre visible dans la cascade : dans son volume, et pas plus petite que quelques texels (au
    // loin, un objet de quelques centimètres ne ferait qu'un point flou de la carte : il n'y est pas dessiné).
    bool MeshRenderer::CastsShadow(const Math::Vector3f& center, float radius, std::size_t cascade) const {
        constexpr float MinTexels = 1.5f; // rayon, en texels de la carte
        return radius >= MinTexels * m_cascades[cascade].texelSize && InCascade(center, radius, cascade);
    }

    bool MeshRenderer::CastsAnyShadow(const Math::Vector3f& center, float radius) const {
        for (std::size_t c = 0; c < ShadowCascadeCount; ++c) {
            if (CastsShadow(center, radius, c)) {
                return true;
            }
        }
        return false;
    }

    MeshRenderer::PlanetTileStats MeshRenderer::GetPlanetTileStats() const {
        PlanetTileStats stats{.drawn = static_cast<std::uint32_t>(m_tileDraws.size()),
                              .stored = m_planets->GetStoredTileCount(),
                              .pending = m_planets->GetPendingTileCount(),
                              .groundLoading = m_groundGroup == nullptr && m_ground.IsLoading()};
        for (const TileDraw& draw : m_tileDraws) {
            stats.ocean += draw.ocean ? 1u : 0u;
        }
        return stats;
    }

    void MeshRenderer::ClearDraws() {
        m_draws.clear();
        m_tileDraws.clear();
        m_tilePasses = {};
        m_modelPasses = {};
    }

    // Commandes indirectes des tuiles de chaque passe, groupées par page (un appel par page), de près à loin dans
    // chacune (le sol proche cache le lointain avant qu'il soit éclairé). false si le buffer ne peut grandir.
    bool MeshRenderer::BuildTileCommands(std::uint32_t frameIndex) {
        m_tileCommands.clear();
        m_tileBatches.clear();
        m_tilePasses = {};
        const RHI::Buffer* indices = m_planets->GetIndices();
        if (m_tileDraws.empty() || !indices) {
            return true;
        }
        std::ranges::sort(m_tileDraws, [](const TileDraw& a, const TileDraw& b) {
            const std::uint32_t pageA = PlanetTileCompute::GetPage(a.tile);
            const std::uint32_t pageB = PlanetTileCompute::GetPage(b.tile);
            return pageA != pageB ? pageA < pageB : a.distance < b.distance;
        });
        const std::uint32_t firstData = static_cast<std::uint32_t>(m_drawData.size());
        const std::uint32_t indexCount = m_planets->GetIndexCount();
        std::array<std::size_t, TilePassCount + 1> passStarts{};
        for (std::size_t pass = 0; pass < TilePassCount; ++pass) {
            passStarts[pass] = m_tileBatches.size();
            for (const TileDraw& draw : m_tileDraws) {
                const bool drawn = pass == GroundPass ? true
                                 : pass == OceanPass  ? draw.ocean
                                                      : InCascade(draw.center, draw.radius, pass - 2);
                if (!drawn) {
                    continue;
                }
                const std::uint32_t page = PlanetTileCompute::GetPage(draw.tile);
                if (m_tileBatches.size() == passStarts[pass] || m_tileBatches.back().page != page) {
                    m_tileBatches.push_back({page, static_cast<std::uint32_t>(m_tileCommands.size()), 0});
                }
                ++m_tileBatches.back().count;
                m_tileCommands.push_back({indexCount, 1, 0, static_cast<std::int32_t>(PlanetTileCompute::GetFirstVertex(draw.tile)),
                                          firstData + draw.data});
            }
        }
        passStarts[TilePassCount] = m_tileBatches.size();

        std::unique_ptr<RHI::Buffer>& buffer = m_indirectBuffers[frameIndex];
        const std::uint64_t bytes = m_tileCommands.size() * sizeof(RHI::DrawIndexedCommand);
        if (!buffer || buffer->GetSize() < bytes) {
            auto grown = m_device.CreateBuffer({std::max(std::bit_ceil(bytes), MinIndirectBufferSize), RHI::BufferUsage::Indirect,
                                                RHI::MemoryType::Upload});
            if (!grown) {
                Log::Error("{}", grown.error());
                return false;
            }
            buffer = std::move(*grown);
        }
        buffer->Write(m_tileCommands.data(), bytes);
        for (std::size_t pass = 0; pass < TilePassCount; ++pass) {
            m_tilePasses[pass] = std::span(m_tileBatches).subspan(passStarts[pass], passStarts[pass + 1] - passStarts[pass]);
        }
        return true;
    }

    void MeshRenderer::AddClouds(PlanetGpu& data, const RelativeTransform& transform, const PlanetComponent& planet,
                                 const CloudsComponent& clouds) const {
        const float coverage = std::clamp(clouds.coverage, 0.0f, 1.0f);
        const float opacity = std::clamp(clouds.opacity, 0.0f, 1.0f);
        if (coverage <= 0.0f || opacity <= 0.0f) {
            return;
        }
        const double radius = std::max(planet.radius, 1.0f) + std::max(clouds.altitude, 0.0f);
        // Dérive : l'angle parcouru depuis le lancement, en double (les secondes grandissent sans fin) et ramené à un
        // tour pour le float. Vers l'est : autour de +y, dans le sens direct vu du pôle nord.
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
        const double drift = std::fmod(seconds * static_cast<double>(clouds.speed) / radius, 2.0 * std::numbers::pi);
        data.clouds = {static_cast<float>(transform.position.x), static_cast<float>(transform.position.y),
                       static_cast<float>(transform.position.z), static_cast<float>(radius)};
        data.cloudRotation = {transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w};
        data.cloudShape = {coverage, opacity, 1.0f / std::max(clouds.size, 100.0f), static_cast<float>(drift)};
        data.cloudColor = {clouds.color.r, clouds.color.g, clouds.color.b, std::max(clouds.thickness, 10.0f)};
    }

    void MeshRenderer::BindPipeline(RHI::CommandList& commands, BindState& state, std::uint32_t frameIndex,
                                    const RHI::Pipeline& pipeline) const {
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
    }

    // Range les entités de m_batchEntries en lots (même pipeline, même modèle, même primitive), chacun un appel. Avec
    // nearestFirst, les lots passent dans l'ordre de leur entité la plus proche et chaque lot de près à loin : ce qu'ils
    // cachent échoue au test de profondeur avant d'être éclairé (coque d'un vaisseau autour de la caméra d'abord).
    void MeshRenderer::AddBatches(bool nearestFirst) {
        std::ranges::sort(m_batchEntries, [](const BatchEntry& a, const BatchEntry& b) {
            if (a.pipeline != b.pipeline) {
                return std::less<>{}(a.pipeline, b.pipeline);
            }
            if (a.model != b.model) {
                return std::less<>{}(a.model, b.model);
            }
            return a.primitive != b.primitive ? a.primitive < b.primitive : a.distance < b.distance;
        });
        m_batchGroups.clear();
        for (std::uint32_t i = 0; i < m_batchEntries.size(); ++i) {
            const BatchEntry& entry = m_batchEntries[i];
            const bool same = !m_batchGroups.empty() && [&] {
                const BatchEntry& first = m_batchEntries[m_batchGroups.back().first];
                return first.pipeline == entry.pipeline && first.model == entry.model && first.primitive == entry.primitive;
            }();
            if (!same) {
                m_batchGroups.push_back({i, 0, entry.distance});
            }
            ++m_batchGroups.back().count;
        }
        if (nearestFirst) {
            std::ranges::sort(m_batchGroups, {}, &BatchGroup::distance);
        }
        for (const BatchGroup& group : m_batchGroups) {
            for (std::uint32_t i = group.first; i < group.first + group.count; ++i) {
                m_instances.push_back(m_batchEntries[i].draw);
            }
            AddBatch(m_batchEntries[group.first], group.count);
        }
        m_batchEntries.clear();
    }

    // Un lot pour les `count` dernières entités de m_instances.
    void MeshRenderer::AddBatch(const BatchEntry& entry, std::uint32_t count) {
        const ModelPrimitive& primitive = entry.model->primitives[entry.primitive];
        m_modelBatches.push_back({entry.pipeline, entry.model, entry.primitive, static_cast<std::uint32_t>(m_modelCommands.size())});
        m_modelCommands.push_back({primitive.indexCount, count, primitive.firstIndex, 0,
                                   static_cast<std::uint32_t>(m_instances.size()) - count});
    }

    // Lots de chaque passe des modèles, leurs entités (m_instances) et leurs commandes, envoyés au GPU. false si un
    // buffer ne peut grandir.
    bool MeshRenderer::BuildModelBatches(std::uint32_t frameIndex) {
        m_instances.clear();
        m_modelCommands.clear();
        m_modelBatches.clear();
        m_modelPasses = {};
        std::array<std::size_t, ModelPassCount + 1> passStarts{};

        passStarts[OpaqueModelPass] = m_modelBatches.size();
        for (const std::uint32_t d : m_opaque) {
            const MeshDraw& draw = m_draws[d];
            const Pipelines& pipelines = GetPipelines(draw);
            for (std::uint32_t p = 0; p < draw.model->primitives.size(); ++p) {
                const GpuMaterial& material = draw.model->materials[draw.model->primitives[p].material];
                if (!IsTransparent(draw, material)) {
                    m_batchEntries.push_back({&OpaquePipeline(pipelines, material), draw.model, p, d, draw.distance - draw.radius});
                }
            }
        }
        AddBatches(true);

        // Transparents : dans leur ordre (de loin à près), une entité par lot. Double face : les faces arrière (le fond
        // de l'objet) d'abord, puis les faces avant par-dessus ; exact pour une forme convexe (bulle de verrière, cube).
        passStarts[TransparentModelPass] = m_modelBatches.size();
        for (const TransparentDraw& item : m_transparent) {
            const MeshDraw& draw = m_draws[item.draw];
            const Pipelines& pipelines = GetPipelines(draw);
            if (draw.model->materials[draw.model->primitives[item.primitive].material].doubleSided) {
                m_instances.push_back(item.draw);
                AddBatch({pipelines.transparentBack.get(), draw.model, item.primitive, item.draw, 0.0f}, 1);
            }
            m_instances.push_back(item.draw);
            AddBatch({pipelines.transparent.get(), draw.model, item.primitive, item.draw, 0.0f}, 1);
        }

        // Ombres : toutes les entités qui en font une dans la cascade, même hors champ ou cachées (le personnage vu
        // de l'intérieur). Les transparents laissent passer la lumière (ni ombre colorée ni ombre partielle).
        for (std::uint32_t c = 0; c < ShadowCascadeCount; ++c) {
            passStarts[2 + c] = m_modelBatches.size();
            for (std::uint32_t d = 0; d < m_draws.size(); ++d) {
                const MeshDraw& draw = m_draws[d];
                if (!CastsShadow(draw.center, draw.radius, c)) {
                    continue;
                }
                const Pipelines& pipelines = GetPipelines(draw);
                for (std::uint32_t p = 0; p < draw.model->primitives.size(); ++p) {
                    const GpuMaterial& material = draw.model->materials[draw.model->primitives[p].material];
                    if (!IsTransparent(draw, material)) {
                        m_batchEntries.push_back({&ShadowPipeline(pipelines, material), draw.model, p, d, draw.distance});
                    }
                }
            }
            AddBatches(false);
        }
        passStarts[ModelPassCount] = m_modelBatches.size();

        const std::uint64_t instanceBytes = m_instances.size() * sizeof(std::uint32_t);
        const std::uint64_t commandBytes = m_modelCommands.size() * sizeof(RHI::DrawIndexedCommand);
        if (!GrowBuffer(m_instanceBuffers[frameIndex], frameIndex, InstanceSlot, instanceBytes)) {
            return false;
        }
        std::unique_ptr<RHI::Buffer>& commands = m_modelCommandBuffers[frameIndex];
        if (!commands || commands->GetSize() < commandBytes) {
            auto grown = m_device.CreateBuffer({std::max(std::bit_ceil(commandBytes), MinModelCommandBufferSize),
                                                RHI::BufferUsage::Indirect, RHI::MemoryType::Upload});
            if (!grown) {
                Log::Error("{}", grown.error());
                return false;
            }
            commands = std::move(*grown);
        }
        if (instanceBytes > 0) {
            m_instanceBuffers[frameIndex]->Write(m_instances.data(), instanceBytes);
            commands->Write(m_modelCommands.data(), commandBytes);
        }
        for (std::size_t pass = 0; pass < ModelPassCount; ++pass) {
            m_modelPasses[pass] = std::span(m_modelBatches).subspan(passStarts[pass], passStarts[pass + 1] - passStarts[pass]);
        }
        return true;
    }

    // Les lots d'une passe des modèles : un appel indirect chacun, les changements d'état seulement quand il le faut.
    void MeshRenderer::DrawBatches(RHI::CommandList& commands, BindState& state, std::uint32_t frameIndex, std::size_t pass) const {
        const std::uint32_t cascade = pass >= 2 ? static_cast<std::uint32_t>(pass - 2) : 0u;
        for (const ModelBatch& batch : m_modelPasses[pass]) {
            const GpuModel& model = *batch.model;
            if (&model != state.model) {
                state.model = &model;
                const std::array<const RHI::Buffer*, 2> vertexBuffers{model.vertices.get(), model.skin.get()};
                commands.BindVertexBuffers(0, std::span(vertexBuffers).first(model.skin ? 2 : 1));
                commands.BindIndexBuffer(*model.indices);
            }
            BindPipeline(commands, state, frameIndex, *batch.pipeline);
            const GpuMaterial& material = model.materials[model.primitives[batch.primitive].material];
            const RHI::BindGroup* group = material.group ? material.group.get() : &m_models->GetWhiteGroup();
            if (group != state.group) {
                state.group = group;
                commands.SetBindGroup(0, *group);
            }
            const DrawParams params{material.baseColor, material.emissive, cascade, material.alphaCutoff,
                                    pass == TransparentModelPass ? 1u : 0u, material.metallic, material.roughness,
                                    material.normalScale, material.occlusionStrength, 0u, {}};
            commands.PushConstants(&params, sizeof(params));
            commands.DrawIndexedIndirect(*m_modelCommandBuffers[frameIndex], batch.command * sizeof(RHI::DrawIndexedCommand), 1);
        }
    }

    // Les tuiles d'une passe : un appel indirect par page. Le sol et l'eau se teintent et se règlent eux-mêmes
    // (PlanetData) : les paramètres sont les mêmes pour toutes.
    void MeshRenderer::DrawTiles(RHI::CommandList& commands, BindState& state, std::uint32_t frameIndex, std::size_t pass,
                                 const RHI::Pipeline& pipeline) const {
        if (m_tilePasses[pass].empty()) {
            return;
        }
        BindPipeline(commands, state, frameIndex, pipeline);
        const RHI::BindGroup* group = m_groundGroup ? m_groundGroup : &m_models->GetWhiteGroup();
        if (group != state.group) {
            state.group = group;
            commands.SetBindGroup(0, *group);
        }
        const std::uint32_t cascade = pass >= 2 ? static_cast<std::uint32_t>(pass - 2) : 0u;
        const DrawParams params{{1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, cascade, 0.0f, pass == OceanPass ? 1u : 0u,
                                0.0f, 1.0f, 0.0f, 0.0f, 0u, {}};
        commands.PushConstants(&params, sizeof(params));
        const PlanetTileCompute& tiles = m_planets->GetTiles();
        commands.BindIndexBuffer(*m_planets->GetIndices());
        for (const TileBatch& batch : m_tilePasses[pass]) {
            const std::array<const RHI::Buffer*, 2> vertexBuffers{&tiles.GetVertices(batch.page), &tiles.GetMorph(batch.page)};
            commands.BindVertexBuffers(0, vertexBuffers);
            commands.DrawIndexedIndirect(*m_indirectBuffers[frameIndex], batch.first * sizeof(RHI::DrawIndexedCommand), batch.count);
        }
        state.model = nullptr; // les sommets liés ne sont plus ceux d'un modèle
    }

    void MeshRenderer::Compute(RHI::CommandList& commands, std::uint32_t frameIndex) {
        m_planets->Compute(commands, frameIndex);
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
            DrawBatches(commands, state, frameIndex, 2 + c);
            DrawTiles(commands, state, frameIndex, 2 + c, *m_planet.shadowDoubleSided); // jupes des tuiles comprises
        }
        commands.EndRenderPass();
    }

    void MeshRenderer::DrawOpaque(RHI::CommandList& commands, std::uint32_t frameIndex) const {
        BindState state;
        DrawBatches(commands, state, frameIndex, OpaqueModelPass);
    }

    void MeshRenderer::DrawGround(RHI::CommandList& commands, std::uint32_t frameIndex) const {
        BindState state;
        DrawTiles(commands, state, frameIndex, GroundPass, *m_planet.opaque);
    }

    void MeshRenderer::DrawOcean(RHI::CommandList& commands, std::uint32_t frameIndex) const {
        BindState state;
        DrawTiles(commands, state, frameIndex, OceanPass, *m_ocean.transparent);
    }

    void MeshRenderer::DrawClouds(RHI::CommandList& commands, std::uint32_t frameIndex) const {
        if (!m_hasClouds) {
            return;
        }
        BindState state;
        BindPipeline(commands, state, frameIndex, *m_clouds);
        commands.SetBindGroup(0, m_models->GetWhiteGroup());
        const DrawParams params{};
        commands.PushConstants(&params, sizeof(params));
        commands.Draw(3, 0);
    }

    void MeshRenderer::DrawTransparent(RHI::CommandList& commands, std::uint32_t frameIndex) const {
        BindState state;
        DrawBatches(commands, state, frameIndex, TransparentModelPass);
    }
}
