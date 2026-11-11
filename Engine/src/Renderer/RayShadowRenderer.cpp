#include "Renderer/RayShadowRenderer.hpp"

#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <cmath>

namespace Flux {
    namespace {
        // Doit correspondre à RayShadowParams dans shaders/RayShadows.slang.
        struct RayShadowParams
        {
            std::array<float, 4> right;
            std::array<float, 4> up;
            std::array<float, 4> forward;
            std::array<float, 4> toSun;
            std::array<float, 4> size; // largeur, hauteur, mètres par pixel à 1 m, inutilisé
            std::array<std::uint32_t, 4> options; // image (bruit), sens du lissage
        };
        static_assert(sizeof(RayShadowParams) == 96);

        constexpr std::uint32_t DepthSlot = 0;
        constexpr std::uint32_t SunSlot = 1;
        constexpr std::uint32_t TargetSlot = 2;
        constexpr std::uint32_t SourceSlot = 3;
        constexpr std::uint32_t SceneSlot = 4;
        constexpr std::uint32_t GroupSize = 8; // comme dans RayShadows.slang
    }

    std::expected<std::unique_ptr<RayShadowRenderer>, std::string> RayShadowRenderer::Create(RHI::Device& device,
                                                                                               RHI::Format sceneFormat) {
        std::unique_ptr<RayShadowRenderer> renderer(new RayShadowRenderer(device));
        auto pointSampler = device.CreateSampler({.filter = RHI::Filter::Nearest, .mipFilter = RHI::Filter::Nearest,
                                                  .address = RHI::AddressMode::ClampToEdge});
        if (!pointSampler) {
            return std::unexpected(pointSampler.error());
        }
        renderer->m_pointSampler = std::move(*pointSampler);

        constexpr RHI::ShaderStage both = RHI::ShaderStage::Compute | RHI::ShaderStage::Fragment;
        constexpr std::array bindings{
            RHI::Binding{DepthSlot, RHI::BindingType::Texture, RHI::ShaderStage::Compute},
            RHI::Binding{SunSlot, RHI::BindingType::Texture, both},
            RHI::Binding{TargetSlot, RHI::BindingType::StorageTexture, RHI::ShaderStage::Compute},
            RHI::Binding{SourceSlot, RHI::BindingType::Texture, both},
            RHI::Binding{SceneSlot, RHI::BindingType::AccelerationStructure, RHI::ShaderStage::Compute},
        };
        auto layout = device.CreateBindGroupLayout({.bindings = bindings, .maxGroups = RHI::FramesInFlight + 4});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        renderer->m_layout = std::move(*layout);
        for (auto& group : renderer->m_traceGroups) {
            group = device.CreateBindGroup(*renderer->m_layout);
        }
        renderer->m_rowsGroup = device.CreateBindGroup(*renderer->m_layout);
        renderer->m_columnsGroup = device.CreateBindGroup(*renderer->m_layout);
        renderer->m_compositeGroup = device.CreateBindGroup(*renderer->m_layout);
        renderer->m_maskGroup = device.CreateBindGroup(*renderer->m_layout);
        if (!renderer->m_traceGroups.back() || !renderer->m_compositeGroup || !renderer->m_maskGroup) {
            return std::unexpected("allocation des groupes des ombres en rayons");
        }

        const std::array<const RHI::BindGroupLayout*, 1> layouts{renderer->m_layout.get()};
        const auto shader = Platform::GetExecutableDirectory() / "shaders" / "RayShadows.spv";
        auto trace = device.CreateComputePipeline({shader, "computeTrace", layouts, sizeof(RayShadowParams)});
        auto filter = device.CreateComputePipeline({shader, "computeFilter", layouts, sizeof(RayShadowParams)});
        if (!trace || !filter) {
            return std::unexpected(!trace ? trace.error() : filter.error());
        }
        renderer->m_trace = std::move(*trace);
        renderer->m_filter = std::move(*filter);

        RHI::PipelineDesc desc;
        desc.shaderPath = shader;
        desc.vertexEntryPoint = "vertexComposite";
        desc.fragmentEntryPoint = "fragmentComposite";
        desc.bindGroupLayouts = layouts;
        desc.colorFormat = sceneFormat;
        desc.blend = RHI::BlendMode::Additive; // valeur négative : la lumière du soleil retirée là où elle n'arrive pas
        auto composite = device.CreatePipeline(desc);
        if (!composite) {
            return std::unexpected(composite.error());
        }
        renderer->m_composite = std::move(*composite);
        desc.fragmentEntryPoint = "fragmentMask";
        desc.blend = RHI::BlendMode::Opaque;
        auto maskView = device.CreatePipeline(desc);
        if (!maskView) {
            return std::unexpected(maskView.error());
        }
        renderer->m_maskView = std::move(*maskView);
        return renderer;
    }

    void RayShadowRenderer::Bind(const Targets& targets) {
        m_targets = targets;
        for (std::size_t f = 0; f < m_traceGroups.size(); ++f) {
            m_traceGroups[f]->SetTexture(DepthSlot, *targets.depth, *m_pointSampler);
            m_traceGroups[f]->SetTexture(SunSlot, *targets.sun, *m_pointSampler);
            m_traceGroups[f]->SetStorageTexture(TargetSlot, *targets.rays);
        }
        m_rowsGroup->SetTexture(DepthSlot, *targets.depth, *m_pointSampler);
        m_rowsGroup->SetTexture(SourceSlot, *targets.rays, *m_pointSampler);
        m_rowsGroup->SetStorageTexture(TargetSlot, *targets.rows);
        m_columnsGroup->SetTexture(DepthSlot, *targets.depth, *m_pointSampler);
        m_columnsGroup->SetTexture(SourceSlot, *targets.rows, *m_pointSampler);
        m_columnsGroup->SetStorageTexture(TargetSlot, *targets.mask);
        m_compositeGroup->SetTexture(SunSlot, *targets.sun, *m_pointSampler);
        m_compositeGroup->SetTexture(SourceSlot, *targets.mask, *m_pointSampler);
        m_maskGroup->SetTexture(SunSlot, *targets.sun, *m_pointSampler);
        m_maskGroup->SetTexture(SourceSlot, *targets.rays, *m_pointSampler);
    }

    void RayShadowRenderer::Run(RHI::CommandList& commands, const RHI::Pipeline& pipeline, const RHI::BindGroup& group,
                                const MeshRenderer::RayShadowView& view, std::uint32_t direction) const {
        const RHI::Extent extent = m_targets.depth->GetExtent();
        // Hauteur du champ à 1 m (deux fois la longueur de `up`) sur la hauteur de l'image.
        const float upLength = std::sqrt(view.up[0] * view.up[0] + view.up[1] * view.up[1] + view.up[2] * view.up[2]);
        const RayShadowParams params{view.right, view.up, view.forward, view.toSun,
                                     {static_cast<float>(extent.width), static_cast<float>(extent.height),
                                      2.0f * upLength / static_cast<float>(std::max(extent.height, 1u)), 0.0f},
                                     {view.noiseFrame, direction, 0u, 0u}};
        commands.BindPipeline(pipeline);
        commands.SetBindGroup(0, group);
        commands.PushConstants(&params, sizeof(params));
        commands.Dispatch((extent.width + GroupSize - 1) / GroupSize, (extent.height + GroupSize - 1) / GroupSize);
    }

    void RayShadowRenderer::Trace(RHI::CommandList& commands, std::uint32_t frameIndex, const RHI::AccelerationStructure& scene,
                                  const MeshRenderer::RayShadowView& view) {
        // La structure de l'image est refaite plus grande de temps en temps : liée de nouveau (groupe de cette image,
        // que le GPU n'utilise plus).
        if (m_boundScene[frameIndex] != &scene) {
            m_traceGroups[frameIndex]->SetAccelerationStructure(SceneSlot, scene);
            m_boundScene[frameIndex] = &scene;
        }
        Run(commands, *m_trace, *m_traceGroups[frameIndex], view, 0);
    }

    void RayShadowRenderer::FilterRows(RHI::CommandList& commands, const MeshRenderer::RayShadowView& view) const {
        Run(commands, *m_filter, *m_rowsGroup, view, 0);
    }

    void RayShadowRenderer::FilterColumns(RHI::CommandList& commands, const MeshRenderer::RayShadowView& view) const {
        Run(commands, *m_filter, *m_columnsGroup, view, 1);
    }

    void RayShadowRenderer::Composite(RHI::CommandList& commands) const {
        commands.BindPipeline(m_showMask ? *m_maskView : *m_composite);
        commands.SetBindGroup(0, m_showMask ? *m_maskGroup : *m_compositeGroup);
        commands.Draw(3, 0);
    }
}
