#include "Renderer/LightingRenderer.hpp"

#include "Flux/Platform/Filesystem.hpp"
#include "Renderer/MeshRenderer.hpp"

namespace Flux {
    namespace {
        // Doit correspondre à DrawParams dans shaders/Mesh.slang (MeshRenderer.cpp), dont Lighting.slang lit ses
        // réglages : color.x, plan proche de la caméra ; cascade, vue de débogage (GBufferView).
        struct LightingParams
        {
            std::array<float, 4> color;
            std::array<float, 4> emissive;
            std::uint32_t view; // DrawParams::cascade
            std::array<std::uint32_t, 11> unused;
        };
        static_assert(sizeof(LightingParams) == 80);

        constexpr std::uint32_t DepthSlot = 0;
        constexpr std::uint32_t BaseSlot = 1;
        constexpr std::uint32_t NormalSlot = 2;
        constexpr std::uint32_t SurfaceSlot = 3;
        constexpr std::uint32_t ColorSlot = 4;
        constexpr std::uint32_t SunSlot = 5;
        constexpr std::uint32_t GroupSize = 8; // comme dans Lighting.slang
    }

    std::expected<std::unique_ptr<LightingRenderer>, std::string> LightingRenderer::Create(RHI::Device& device,
                                                                                            const MeshRenderer& meshes) {
        std::unique_ptr<LightingRenderer> renderer(new LightingRenderer(device, meshes));
        renderer->m_rayTracing = meshes.HasRayTracing(); // mêmes variantes *Rays que les shaders de la scène
        auto pointSampler = device.CreateSampler({.filter = RHI::Filter::Nearest, .mipFilter = RHI::Filter::Nearest,
                                                  .address = RHI::AddressMode::ClampToEdge});
        if (!pointSampler) {
            return std::unexpected(pointSampler.error());
        }
        renderer->m_pointSampler = std::move(*pointSampler);

        constexpr RHI::ShaderStage compute = RHI::ShaderStage::Compute;
        constexpr std::array bindings{
            RHI::Binding{DepthSlot, RHI::BindingType::Texture, compute},
            RHI::Binding{BaseSlot, RHI::BindingType::Texture, compute},
            RHI::Binding{NormalSlot, RHI::BindingType::Texture, compute},
            RHI::Binding{SurfaceSlot, RHI::BindingType::Texture, compute},
            RHI::Binding{ColorSlot, RHI::BindingType::StorageTexture, compute},
            RHI::Binding{SunSlot, RHI::BindingType::StorageTexture, compute},
        };
        // La lumière du soleil à part n'existe qu'avec le ray tracing (LightingRays.slang).
        const std::span<const RHI::Binding> used = renderer->m_rayTracing ? std::span(bindings) : std::span(bindings).first(bindings.size() - 1);
        auto layout = device.CreateBindGroupLayout({.bindings = used, .maxGroups = 1});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        renderer->m_layout = std::move(*layout);
        renderer->m_group = device.CreateBindGroup(*renderer->m_layout);
        if (!renderer->m_group) {
            return std::unexpected("allocation du groupe de l'éclairage");
        }

        const std::array<const RHI::BindGroupLayout*, 3> scene = meshes.GetSceneLayouts();
        const std::array<const RHI::BindGroupLayout*, 4> layouts{scene[0], scene[1], scene[2], renderer->m_layout.get()};
        const auto shader = Platform::GetExecutableDirectory() / "shaders"
                          / (renderer->m_rayTracing ? "LightingRays.spv" : "Lighting.spv");
        auto pipeline = device.CreateComputePipeline({shader, "computeLighting", layouts, sizeof(LightingParams)});
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        renderer->m_lighting = std::move(*pipeline);
        return renderer;
    }

    void LightingRenderer::Bind(const Targets& targets) {
        m_extent = targets.depth->GetExtent();
        m_group->SetTexture(DepthSlot, *targets.depth, *m_pointSampler);
        m_group->SetTexture(BaseSlot, *targets.base, *m_pointSampler);
        m_group->SetTexture(NormalSlot, *targets.normals, *m_pointSampler);
        m_group->SetTexture(SurfaceSlot, *targets.surface, *m_pointSampler);
        m_group->SetStorageTexture(ColorSlot, *targets.color);
        if (m_rayTracing) {
            m_group->SetStorageTexture(SunSlot, *targets.sun);
        }
    }

    void LightingRenderer::Draw(RHI::CommandList& commands, std::uint32_t frameIndex, float nearPlane) const {
        const LightingParams params{{nearPlane, 0.0f, 0.0f, 0.0f}, {}, static_cast<std::uint32_t>(m_view), {}};
        commands.BindPipeline(*m_lighting);
        m_meshes.BindScene(commands, frameIndex);
        commands.SetBindGroup(3, *m_group);
        commands.PushConstants(&params, sizeof(params));
        commands.Dispatch((m_extent.width + GroupSize - 1) / GroupSize, (m_extent.height + GroupSize - 1) / GroupSize);
    }
}
