#include "Renderer/LitterRenderer.hpp"

#include "Flux/Platform/Filesystem.hpp"
#include "Renderer/LightingRenderer.hpp"
#include "Renderer/MeshRenderer.hpp"

#include <algorithm>
#include <array>

namespace Flux {
    namespace {
        // Doit correspondre à DrawParams dans shaders/Mesh.slang, dont Litter.slang lit ses réglages : color.x, plan
        // proche de la caméra ; color.y, rayon de la litière (m, à la taille du modèle) ; color.zw, début et 1 / largeur
        // de son effacement (m) ; material, son matériau.
        struct LitterParams
        {
            std::array<float, 4> color;
            std::array<float, 4> emissive;
            std::array<std::uint32_t, 7> unused; // cascade à occlusionStrength
            std::uint32_t material;
            std::array<std::uint32_t, 4> padding; // draw et la fin
        };
        static_assert(sizeof(LitterParams) == 80);

        constexpr std::uint32_t DepthSlot = 0;
        constexpr std::uint32_t NormalSlot = 1;
        constexpr std::uint32_t SurfaceSlot = 2;
        // La litière s'efface sur ce dernier tiers de la liste Near (au-delà, les arbres simplifiés n'en ont pas).
        constexpr float FadeShare = 0.3f;
    }

    std::expected<std::unique_ptr<LitterRenderer>, std::string> LitterRenderer::Create(RHI::Device& device, const MeshRenderer& meshes) {
        std::unique_ptr<LitterRenderer> renderer(new LitterRenderer(device, meshes));
        auto pointSampler = device.CreateSampler({.filter = RHI::Filter::Nearest, .mipFilter = RHI::Filter::Nearest,
                                                  .address = RHI::AddressMode::ClampToEdge});
        if (!pointSampler) {
            return std::unexpected(pointSampler.error());
        }
        renderer->m_pointSampler = std::move(*pointSampler);

        constexpr RHI::ShaderStage fragment = RHI::ShaderStage::Fragment;
        constexpr std::array bindings{
            RHI::Binding{DepthSlot, RHI::BindingType::Texture, fragment},
            RHI::Binding{NormalSlot, RHI::BindingType::Texture, fragment},
            RHI::Binding{SurfaceSlot, RHI::BindingType::Texture, fragment},
        };
        auto layout = device.CreateBindGroupLayout({.bindings = bindings, .maxGroups = 1});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        renderer->m_layout = std::move(*layout);
        renderer->m_group = device.CreateBindGroup(*renderer->m_layout);
        if (!renderer->m_group) {
            return std::unexpected("allocation du groupe de la litière");
        }

        // Coins c d'une boîte de -1 à 1 : x, y, z selon les bits 0, 1, 2. Faces en sens antihoraire vues du dehors.
        constexpr std::array<std::uint32_t, Planetology::VegetationRenderer::LitterIndices> box{
            1, 3, 7, 1, 7, 5, // +x
            0, 4, 6, 0, 6, 2, // -x
            2, 6, 7, 2, 7, 3, // +y
            0, 1, 5, 0, 5, 4, // -y
            4, 5, 7, 4, 7, 6, // +z
            0, 2, 3, 0, 3, 1, // -z
        };
        auto indices = device.CreateBuffer({sizeof(box), RHI::BufferUsage::Index}, box.data());
        if (!indices) {
            return std::unexpected(indices.error());
        }
        renderer->m_indices = std::move(*indices);

        const std::array<const RHI::BindGroupLayout*, 3> scene = meshes.GetSceneLayouts();
        const std::array<const RHI::BindGroupLayout*, 4> layouts{scene[0], scene[1], scene[2], renderer->m_layout.get()};
        RHI::PipelineDesc desc;
        // Mêmes variantes *Rays que les shaders de la scène (groupe 1 de même forme).
        desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / (meshes.HasRayTracing() ? "LitterRays.spv" : "Litter.spv");
        desc.vertexEntryPoint = "vertexLitter";
        desc.fragmentEntryPoint = "fragmentLitter";
        desc.bindGroupLayouts = layouts;
        desc.pushConstantsSize = sizeof(LitterParams);
        desc.colorFormat = LightingRenderer::BaseFormat;
        // Faces arrière seulement : chaque pixel une fois, la caméra dans la boîte comprise ; pas de profondeur, le
        // shader lit celle de la scène.
        desc.cullMode = RHI::CullMode::Front;
        desc.blend = RHI::BlendMode::Decal;
        auto pipeline = device.CreatePipeline(desc);
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        renderer->m_pipeline = std::move(*pipeline);
        return renderer;
    }

    void LitterRenderer::Bind(const Targets& targets) {
        m_group->SetTexture(DepthSlot, *targets.depth, *m_pointSampler);
        m_group->SetTexture(NormalSlot, *targets.normals, *m_pointSampler);
        m_group->SetTexture(SurfaceSlot, *targets.surface, *m_pointSampler);
    }

    void LitterRenderer::Draw(RHI::CommandList& commands, std::uint32_t frameIndex, float nearPlane) const {
        const Planetology::VegetationRenderer& vegetation = m_meshes.GetVegetation();
        if (vegetation.GetLitter().empty()) {
            return;
        }
        commands.BindPipeline(*m_pipeline);
        m_meshes.BindScene(commands, frameIndex);
        commands.SetBindGroup(3, *m_group);
        commands.BindIndexBuffer(*m_indices);
        const float end = vegetation.GetLodDistance();
        const float width = std::max(FadeShare * end, 1.0f);
        for (const Planetology::VegetationRenderer::LitterDraw& litter : vegetation.GetLitter()) {
            const LitterParams params{{nearPlane, litter.radius, end - width, 1.0f / width}, {}, {}, litter.material, {}};
            commands.PushConstants(&params, sizeof(params));
            commands.DrawIndexedIndirect(vegetation.GetCommands(frameIndex), litter.command * sizeof(RHI::DrawIndexedCommand), 1);
        }
    }
}
