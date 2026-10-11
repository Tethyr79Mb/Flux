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
            Math::Matrix4f viewProjection;
            std::array<Math::Matrix4f, ShadowCascadeCount> cascades;
            std::array<float, 4> cascadeEnds;
            std::array<float, 4> cascadeTexels;
            std::array<float, 4> cameraForward;
            std::array<float, 4> toSun;
            std::array<float, 4> sunColor;      // couleur x intensité
            // x : multiplie la texture cube du ciel ; y : niveau de mip de la lumière diffuse ; z : niveau de mip des
            // reflets les plus flous ; w : exposition
            std::array<float, 4> environment;
            std::array<std::uint32_t, 4> lightCount; // x : lumières ; y : volumes intérieurs
            std::array<InteriorData, MaxInteriors> interiors;
            std::array<PlanetGpu, MaxPlanets> planets;
            AtmosphereGpu atmosphere;
        };
        static_assert(sizeof(FrameData) == 432 + MaxInteriors * sizeof(InteriorData) + MaxPlanets * sizeof(PlanetGpu) + sizeof(AtmosphereGpu)
                      && ShadowCascadeCount == 4);


        // Doit correspondre à DrawParams dans shaders/Mesh.slang : envoyé avec chaque primitive. L'indice de l'entité
        // (dans les DrawData) est celui de l'instance dessinée.
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
            std::uint32_t materialOverride;
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

        // Tailles minimales des buffers de l'image : de quoi animer des dizaines de personnages et dessiner des
        // centaines d'entités sans réallouer.
        constexpr std::uint64_t MinJointBufferSize = 64 * 1024;
        constexpr std::uint64_t MinDrawBufferSize = 64 * 1024;
        constexpr std::uint64_t MinLightBufferSize = 16 * 1024;
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
                                                                                   RHI::Format depthFormat) {
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
                || !renderer->GrowBuffer(renderer->m_lightBuffers[f], f, LightSlot, MinLightBufferSize)) {
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
            const char* fragment = "fragmentMain";
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

            // Carte d'ombres : profondeur seule, la plus proche du soleil (0 côté soleil). Les deux faces : une
            // coque ouverte ou un matériau double face fait aussi de l'ombre. Le décalage de profondeur (selon la
            // pente) évite qu'une surface s'ombre elle-même par endroits (« acné »), avec le décalage le long de la
            // normale fait à la lecture.
            desc.vertexEntryPoint = kind.shadowVertex;
            desc.vertexAttributes = kind.shadowAttributes;
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

            desc.vertexEntryPoint = kind.vertex;
            desc.vertexAttributes = kind.attributes;
            desc.fragmentEntryPoint = kind.fragment;
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
        if (auto result = createAll(m_static, {staticBuffers, staticAttributes, staticShadow, "vertexMain", "vertexShadow"}); !result) {
            return result;
        }
        if (auto result = createAll(m_skinned, {skinnedBuffers, skinnedAttributes, skinnedShadow, "vertexSkinned", "vertexShadowSkinned"});
            !result) {
            return result;
        }
        if (auto result = createAll(m_planet, {planetBuffers, planetAttributes, planetShadow, "vertexPlanet", "vertexShadowPlanet",
                                               "fragmentPlanet"});
            !result) {
            return result;
        }
        return createAll(m_ocean, {planetBuffers, oceanAttributes, planetShadow, "vertexOcean", "vertexShadowPlanet", "fragmentOcean"});
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
                               const std::filesystem::path& assetsDirectory, std::uint32_t frameIndex) {
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
        m_planetData.clear();
        // Textures du sol : lues (sur un autre fil) seulement quand la scène a une planète.
        const bool planets = std::ranges::any_of(m_relativeTransforms, [&scene](const RelativeTransform& transform) {
            return scene.TryGet<PlanetComponent>(transform.entity) != nullptr;
        });
        m_groundGroup = planets ? m_ground.Get(m_device, *m_models, assetsDirectory) : nullptr;
        const ViewCone view(camera, aspect);
        m_planets->BeginFrame(frameIndex);
        for (const RelativeTransform& transform : m_relativeTransforms) {
            if (const auto* planet = scene.TryGet<PlanetComponent>(transform.entity)) {
                AddPlanet(transform, *planet, view);
            }
            if (const auto* light = scene.TryGet<LightComponent>(transform.entity)) {
                AddLight(camera, aspect, transform, *light);
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
            m_drawData.push_back({world,
                                  {{{x.x, x.y, x.z, 0.0f}, {y.x, y.y, y.z, std::bit_cast<float>(firstJoint)}, {z.x, z.y, z.z, 0.0f}}},
                                  rim, {}});

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
            m_draws.push_back({model, tint, emissive, metallic, roughness, materialOverride, relativeCenter, radius,
                               Math::Length(relativeCenter), !IsInside(scene, transform.entity, hidden), inView});
        }
        // Tuiles manquantes des planètes (calculées au début de l'image suivante, dessinées à partir d'elle).
        m_planets->EndFrame(frameIndex);
        if (!m_boundEnvironment[frameIndex]) {
            ClearDraws(); // pas de texture du ciel (mémoire épuisée) : rien plutôt qu'une lecture invalide
        }
        Upload(camera, aspect, sky, frameIndex);
        if (!BuildTileCommands(frameIndex)) {
            m_tileDraws.clear();
            m_tilePasses = {};
        }

        // Transparents de l'arrière vers l'avant : chacun se mélange à ce qui est derrière lui. Trié par entité (le
        // centre de sa boîte) : deux transparents qui s'entrecroisent peuvent se superposer dans le mauvais ordre.
        // Opaques du plus proche au plus loin : ce qu'ils cachent échoue au test de profondeur avant d'être éclairé.
        // Hors champ : ombre seulement.
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
        // Distance au bord de la sphère : la coque d'un vaisseau autour de la caméra passe en premier.
        std::sort(m_opaque.begin(), m_opaque.end(), [this](std::uint32_t a, std::uint32_t b) {
            return m_draws[a].distance - m_draws[a].radius < m_draws[b].distance - m_draws[b].radius;
        });
        // Stable : les primitives d'une même entité gardent l'ordre du fichier.
        std::stable_sort(m_transparent.begin(), m_transparent.end(), [this](const TransparentDraw& a, const TransparentDraw& b) {
            return m_draws[a.draw].distance > m_draws[b.draw].distance;
        });
    }

    // Tuiles de la planète à dessiner, relatives à la caméra : leur centre est calculé en double (PlanetRenderer), le
    // GPU ne voit que des sommets proches de ce centre.
    void MeshRenderer::AddPlanet(const RelativeTransform& transform, const PlanetComponent& planet, const ViewCone& view) {
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
            m_planetData.push_back({{std::max(planet.radius, 1.0f), planet.relief, planet.snowLevel * 0.01f * planet.relief,
                                     planet.sandLevel * 0.01f * planet.relief},
                                    material(planet.color, planet.rockTint, roughness),
                                    material(planet.grassColor, planet.grassTint, 0.95f),
                                    material(planet.sandColor, planet.sandTint, 0.9f),
                                    material(planet.snowColor, planet.snowTint, 0.55f),
                                    {planet.waterColor.r, planet.waterColor.g, planet.waterColor.b, 0.0f},
                                    material(planet.soilColor, planet.soilTint, 0.95f),
                                    {planet.temperature, planet.poleTemperature, std::clamp(planet.humidity, 0.0f, 1.0f),
                                     m_groundGroup ? 1.0f : 0.0f}});
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
            const bool water = ocean && tile.lowest < seaRadius
                            && view.Contains(center, tile.radius + static_cast<float>(seaRadius - tile.lowest));
            m_tileDraws.push_back({tile.tile, static_cast<std::uint32_t>(m_tileData.size()), center, tile.radius,
                                   Math::Length(center) - tile.radius, water});
            m_tileData.push_back({Math::Matrix4f::Translation(center) * rotation,
                                  {{{x.x, x.y, x.z, tile.morphStart}, {y.x, y.y, y.z, 0.0f}, {z.x, z.y, z.z, tile.morphScale}}},
                                  {local.x, local.y, local.z, std::bit_cast<float>(index)},
                                  {wrap(tile.local.x), wrap(tile.local.y), wrap(tile.local.z), 0.0f}});
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
    void MeshRenderer::Upload(const Camera& camera, float aspect, const SkyLighting& sky, std::uint32_t frameIndex) {
        static_assert(sizeof(Math::Matrix4f) == 64 && sizeof(DrawData) == 144 && sizeof(LightData) == 64);
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

        m_cascades = ComputeShadowCascades(camera, aspect, sky.toSun, ShadowCascadeResolution);
        FrameData frame{};
        frame.viewProjection = camera.GetProjectionMatrix(aspect) * camera.GetViewMatrix();
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
                            static_cast<std::uint32_t>(m_planetData.size()), 0};
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

    void MeshRenderer::ClearDraws() {
        m_draws.clear();
        m_tileDraws.clear();
        m_tilePasses = {};
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

    void MeshRenderer::DrawPrimitive(RHI::CommandList& commands, BindState& state, std::uint32_t frameIndex,
                                     std::uint32_t drawIndex, const ModelPrimitive& primitive, const RHI::Pipeline& pipeline,
                                     std::uint32_t cascade) const {
        const MeshDraw& draw = m_draws[drawIndex];
        const GpuModel& model = *draw.model;
        if (&model != state.model) {
            state.model = &model;
            const std::array<const RHI::Buffer*, 2> vertexBuffers{model.vertices.get(), model.skin.get()};
            commands.BindVertexBuffers(0, std::span(vertexBuffers).first(model.skin ? 2 : 1));
            commands.BindIndexBuffer(*model.indices);
        }
        BindPipeline(commands, state, frameIndex, pipeline);
        const GpuMaterial& material = model.materials[primitive.material];
        const RHI::BindGroup* group = material.group ? material.group.get() : &m_models->GetWhiteGroup();
        if (group != state.group) {
            state.group = group;
            commands.SetBindGroup(0, *group);
        }
        const DrawParams params{{material.baseColor.r * draw.tint.r, material.baseColor.g * draw.tint.g,
                                 material.baseColor.b * draw.tint.b, material.baseColor.a * draw.tint.a},
                                {material.emissive.r + draw.emissive.r, material.emissive.g + draw.emissive.g,
                                 material.emissive.b + draw.emissive.b, 0.0f},
                                cascade, material.alphaCutoff, IsTransparent(draw, material) ? 1u : 0u,
                                draw.materialOverride ? draw.metallic : material.metallic * draw.metallic,
                                draw.materialOverride ? draw.roughness : material.roughness * draw.roughness,
                                material.normalScale, material.occlusionStrength, draw.materialOverride ? 1u : 0u, {}};
        commands.PushConstants(&params, sizeof(params));
        commands.DrawIndexed(primitive.indexCount, primitive.firstIndex, drawIndex);
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
                                0.0f, 1.0f, 0.0f, 0.0f, 1u, {}};
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
            for (std::uint32_t d = 0; d < m_draws.size(); ++d) {
                const MeshDraw& draw = m_draws[d];
                if (!InCascade(draw.center, draw.radius, c)) {
                    continue;
                }
                const RHI::Pipeline& pipeline = *GetPipelines(draw).shadow;
                for (const ModelPrimitive& primitive : draw.model->primitives) {
                    // Les transparents laissent passer la lumière (ni ombre colorée ni ombre partielle pour l'instant).
                    if (!IsTransparent(draw, draw.model->materials[primitive.material])) {
                        DrawPrimitive(commands, state, frameIndex, d, primitive, pipeline, c);
                    }
                }
            }
            DrawTiles(commands, state, frameIndex, 2 + c, *m_planet.shadow);
        }
        commands.EndRenderPass();
    }

    void MeshRenderer::DrawOpaque(RHI::CommandList& commands, std::uint32_t frameIndex) const {
        // Changements d'état seulement quand il le faut. L'ordre de près à loin (Prepare) sépare parfois les entités
        // d'un même modèle : quelques changements de plus, contre moins de pixels éclairés pour rien.
        BindState state;
        for (const std::uint32_t d : m_opaque) {
            const MeshDraw& draw = m_draws[d];
            const Pipelines& pipelines = GetPipelines(draw);
            for (const ModelPrimitive& primitive : draw.model->primitives) {
                const GpuMaterial& material = draw.model->materials[primitive.material];
                if (!IsTransparent(draw, material)) {
                    DrawPrimitive(commands, state, frameIndex, d, primitive,
                                  material.doubleSided ? *pipelines.opaqueDoubleSided : *pipelines.opaque);
                }
            }
        }
        // Le sol des planètes après les entités : ce qu'elles en cachent (un vaisseau posé dessus) n'est pas éclairé.
        DrawTiles(commands, state, frameIndex, GroundPass, *m_planet.opaque);
    }

    void MeshRenderer::DrawTransparent(RHI::CommandList& commands, std::uint32_t frameIndex) const {
        BindState state;
        // L'eau d'abord : vue d'au-dessus de la mer, tout transparent devant elle est plus près (une verrière).
        DrawTiles(commands, state, frameIndex, OceanPass, *m_ocean.transparent);
        for (const TransparentDraw& item : m_transparent) {
            const MeshDraw& draw = m_draws[item.draw];
            const Pipelines& pipelines = GetPipelines(draw);
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
