#include "Renderer/PostRenderer.hpp"

#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <cmath>

namespace Flux {
    namespace {
        constexpr std::uint32_t SourceSlot = 0;
        constexpr std::uint32_t OtherSlot = 1;
        // Anticrénelage temporel (Temporal.slang) : SourceSlot la scène, OtherSlot l'historique, puis :
        constexpr std::uint32_t DepthSlot = 2;
        constexpr std::uint32_t MotionSlot = 3;
        // Part de l'image actuelle dans le mélange : la moyenne d'une dizaine d'images. Plus haut, les bords lissent
        // moins ; plus bas, ce qui change (lumière, objet qui apparaît) traîne plus longtemps.
        constexpr float CurrentWeight = 0.1f;
        // Le plus petit niveau du bloom garde au moins ce côté (pixels) : plus petit, il ne fait plus que du gris.
        constexpr std::uint32_t MinLevelSize = 8;
    }

    std::expected<std::unique_ptr<PostRenderer>, std::string> PostRenderer::Create(RHI::Device& device, RHI::Format outputFormat,
                                                                                    RHI::Format depthFormat) {
        std::unique_ptr<PostRenderer> post(new PostRenderer(device));
        // Bords répétés : le flou n'apporte pas de lumière du côté opposé de l'image.
        auto sampler = device.CreateSampler({.filter = RHI::Filter::Linear, .mipFilter = RHI::Filter::Nearest,
                                             .address = RHI::AddressMode::ClampToEdge});
        if (!sampler) {
            return std::unexpected(sampler.error());
        }
        post->m_sampler = std::move(*sampler);
        constexpr std::array bindings{
            RHI::Binding{SourceSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
            RHI::Binding{OtherSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
        };
        auto layout = device.CreateBindGroupLayout({.bindings = bindings, .maxGroups = 2 * MaxLevels + 2 * OutputCount});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        post->m_layout = std::move(*layout);

        const RHI::BindGroupLayout* layouts[] = {post->m_layout.get()};
        const auto create = [&](std::unique_ptr<RHI::Pipeline>& out, const char* entryPoint, RHI::Format color,
                                RHI::Format depth) -> std::expected<void, std::string> {
            RHI::PipelineDesc desc;
            desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Post.spv";
            desc.fragmentEntryPoint = entryPoint;
            desc.bindGroupLayouts = layouts;
            desc.pushConstantsSize = sizeof(PostParams);
            desc.colorFormat = color;
            desc.depthFormat = depth;
            auto pipeline = device.CreatePipeline(desc);
            if (!pipeline) {
                return std::unexpected(pipeline.error());
            }
            out = std::move(*pipeline);
            return {};
        };
        auto pointSampler = device.CreateSampler({.filter = RHI::Filter::Nearest, .mipFilter = RHI::Filter::Nearest,
                                                  .address = RHI::AddressMode::ClampToEdge});
        if (!pointSampler) {
            return std::unexpected(pointSampler.error());
        }
        post->m_pointSampler = std::move(*pointSampler);
        constexpr std::array temporalBindings{
            RHI::Binding{SourceSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
            RHI::Binding{OtherSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
            RHI::Binding{DepthSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
            RHI::Binding{MotionSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
        };
        auto temporalLayout = device.CreateBindGroupLayout({.bindings = temporalBindings, .maxGroups = 2});
        if (!temporalLayout) {
            return std::unexpected(temporalLayout.error());
        }
        post->m_temporalLayout = std::move(*temporalLayout);
        const RHI::BindGroupLayout* temporalLayouts[] = {post->m_temporalLayout.get()};
        RHI::PipelineDesc temporalDesc;
        temporalDesc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Temporal.spv";
        temporalDesc.fragmentEntryPoint = "fragmentTemporal";
        temporalDesc.bindGroupLayouts = temporalLayouts;
        temporalDesc.pushConstantsSize = sizeof(TemporalParams);
        temporalDesc.colorFormat = SceneFormat;
        auto temporal = device.CreatePipeline(temporalDesc);
        if (!temporal) {
            return std::unexpected(temporal.error());
        }
        post->m_temporal = std::move(*temporal);

        constexpr RHI::Binding gridBinding{0, RHI::BindingType::Texture, RHI::ShaderStage::Fragment};
        auto gridLayout = device.CreateBindGroupLayout({.bindings = {&gridBinding, 1}, .maxGroups = 1});
        if (!gridLayout) {
            return std::unexpected(gridLayout.error());
        }
        post->m_gridLayout = std::move(*gridLayout);
        const RHI::BindGroupLayout* gridLayouts[] = {post->m_gridLayout.get()};
        RHI::PipelineDesc gridDesc;
        gridDesc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Grid.spv";
        gridDesc.bindGroupLayouts = gridLayouts;
        gridDesc.pushConstantsSize = sizeof(GridParams);
        gridDesc.colorFormat = outputFormat;
        gridDesc.depthFormat = depthFormat;
        gridDesc.blend = RHI::BlendMode::Alpha;
        auto grid = device.CreatePipeline(gridDesc);
        if (!grid) {
            return std::unexpected(grid.error());
        }
        post->m_grid = std::move(*grid);

        for (auto result : {create(post->m_downsampleFirst, "fragmentDownsampleFirst", SceneFormat, RHI::Format::Undefined),
                            create(post->m_downsample, "fragmentDownsample", SceneFormat, RHI::Format::Undefined),
                            create(post->m_upsample, "fragmentUpsample", SceneFormat, RHI::Format::Undefined),
                            create(post->m_composite, "fragmentComposite", outputFormat, depthFormat)}) {
            if (!result) {
                return std::unexpected(result.error());
            }
        }
        return post;
    }

    std::expected<void, std::string> PostRenderer::Resize(RHI::Extent extent) {
        extent.width = std::max(extent.width, 1u);
        extent.height = std::max(extent.height, 1u);
        if (m_scene && m_extent == extent) {
            return {};
        }
        m_device.WaitIdle(); // les images en vol les utilisent peut-être encore
        // Groupes libérés avant les textures qu'ils désignent, et tout libéré avant d'allouer à la nouvelle taille.
        for (std::uint32_t o = 0; o < OutputCount; ++o) {
            m_compositeGroups[o].reset();
            m_firstDownGroups[o].reset();
        }
        for (auto& group : m_temporalGroups) {
            group.reset();
        }
        m_gridGroup.reset();
        for (auto& history : m_history) {
            history.reset();
        }
        m_motion.reset();
        m_historyValid = false;
        m_historyCleared = false;
        for (std::uint32_t i = 0; i < MaxLevels; ++i) {
            m_downGroups[i].reset();
            m_upGroups[i].reset();
            m_down[i].reset();
            m_up[i].reset();
        }
        m_scene.reset();
        m_depth.reset();
        m_extent = extent;

        // Profondeur lisible : l'anticrénelage y retrouve où était chaque point.
        auto scene = m_device.CreateRenderTarget(SceneFormat, extent, true);
        auto depth = m_device.CreateRenderTarget(DepthFormat, extent, true);
        auto motion = m_device.CreateRenderTarget(MotionFormat, extent, true);
        if (!scene || !depth || !motion) {
            return std::unexpected(!scene ? scene.error() : !depth ? depth.error() : motion.error());
        }
        m_scene = std::move(*scene);
        m_depth = std::move(*depth);
        m_motion = std::move(*motion);
        for (auto& history : m_history) {
            auto created = m_device.CreateRenderTarget(SceneFormat, extent, true);
            if (!created) {
                return std::unexpected(created.error());
            }
            history = std::move(*created);
        }

        // Niveaux : la moitié de la vue, puis la moitié de la moitié... tant que le petit côté garde MinLevelSize.
        m_levels = 0;
        RHI::Extent size = extent;
        while (m_levels < MaxLevels && std::min(size.width, size.height) / 2 >= MinLevelSize) {
            size = {size.width / 2, size.height / 2};
            auto down = m_device.CreateRenderTarget(SceneFormat, size, true);
            if (!down) {
                return std::unexpected(down.error());
            }
            m_down[m_levels] = std::move(*down);
            if (m_levels > 0) {
                auto up = m_device.CreateRenderTarget(SceneFormat, m_down[m_levels - 1]->GetExtent(), true);
                if (!up) {
                    return std::unexpected(up.error());
                }
                m_up[m_levels - 1] = std::move(*up);
            }
            ++m_levels;
        }
        if (m_levels > 0) {
            // Le dernier niveau n'a pas de remontée : c'est sa réduction.
            m_up[m_levels - 1].reset();
        }

        const auto group = [&](const RHI::Texture& source, const RHI::Texture& other) {
            std::unique_ptr<RHI::BindGroup> created = m_device.CreateBindGroup(*m_layout);
            if (created) {
                created->SetTexture(SourceSlot, source, *m_sampler);
                created->SetTexture(OtherSlot, other, *m_sampler);
            }
            return created;
        };
        for (std::uint32_t i = 1; i < m_levels; ++i) {
            m_downGroups[i] = group(*m_down[i - 1], *m_down[i - 1]);
            if (!m_downGroups[i]) {
                return std::unexpected("création des groupes du bloom");
            }
        }
        for (std::uint32_t i = 0; i + 1 < m_levels; ++i) {
            const RHI::Texture& smaller = i + 2 < m_levels ? *m_up[i + 1] : *m_down[i + 1];
            m_upGroups[i] = group(smaller, *m_down[i]);
            if (!m_upGroups[i]) {
                return std::unexpected("création des groupes du bloom");
            }
        }
        for (std::uint32_t o = 0; o < OutputCount; ++o) {
            const RHI::Texture& output = GetOutput(o);
            // Sans niveau de bloom, l'image finale lit deux fois la sortie (force du bloom à 0).
            const RHI::Texture* bloom = m_levels >= 2 ? m_up[0].get() : m_levels == 1 ? m_down[0].get() : &output;
            m_firstDownGroups[o] = group(output, output);
            m_compositeGroups[o] = group(output, *bloom);
            if (!m_firstDownGroups[o] || !m_compositeGroups[o]) {
                return std::unexpected("création des groupes de l'image finale");
            }
        }
        for (std::uint32_t i = 0; i < 2; ++i) {
            m_temporalGroups[i] = m_device.CreateBindGroup(*m_temporalLayout);
            if (!m_temporalGroups[i]) {
                return std::unexpected("création des groupes de l'anticrénelage");
            }
            m_temporalGroups[i]->SetTexture(SourceSlot, *m_scene, *m_sampler);
            m_temporalGroups[i]->SetTexture(OtherSlot, *m_history[1 - i], *m_sampler);
            m_temporalGroups[i]->SetTexture(DepthSlot, *m_depth, *m_pointSampler);
            m_temporalGroups[i]->SetTexture(MotionSlot, *m_motion, *m_pointSampler);
        }
        m_gridGroup = m_device.CreateBindGroup(*m_gridLayout);
        if (!m_gridGroup) {
            return std::unexpected("création du groupe de la grille");
        }
        m_gridGroup->SetTexture(0, *m_depth, *m_pointSampler);
        return {};
    }

    void PostRenderer::DrawPass(RHI::CommandList& commands, const RHI::Pipeline& pipeline, const RHI::Texture& target,
                                const RHI::BindGroup& group, const RHI::Texture& source, float radius) const {
        const RHI::Extent size = target.GetExtent();
        const RHI::Extent from = source.GetExtent();
        commands.BeginRenderPass({.colorTarget = &target});
        commands.SetViewport({0.0f, 0.0f, static_cast<float>(size.width), static_cast<float>(size.height)});
        commands.SetScissor({0, 0, size.width, size.height});
        commands.BindPipeline(pipeline);
        commands.SetBindGroup(0, group);
        const PostParams params{{1.0f / static_cast<float>(from.width), 1.0f / static_cast<float>(from.height), radius, 0.0f},
                                {}};
        commands.PushConstants(&params, sizeof(params));
        commands.Draw(3, 0);
        commands.EndRenderPass();
    }

    const RHI::Texture& PostRenderer::GetOutput(std::uint32_t output) const {
        return output == 0 ? *m_scene : *m_history[output - 1];
    }

    void PostRenderer::ResolveTemporal(RHI::CommandList& commands, bool enabled, const Math::Matrix4f& reprojection,
                                       bool history) {
        if (!enabled) {
            m_output = 0;
            m_historyValid = false;
            return;
        }
        const std::uint32_t target = 1 - m_written;
        // L'historique lu n'a jamais été écrit à la première image : une passe vide l'efface et le rend lisible
        // (sinon son layout reste indéfini et la validation Vulkan refuse de l'échantillonner, même avec un poids nul).
        if (!m_historyCleared) {
            commands.BeginRenderPass({.colorTarget = m_history[1 - target].get()});
            commands.EndRenderPass();
            m_historyCleared = true;
        }
        const TemporalParams params{reprojection,
                                    {1.0f / static_cast<float>(m_extent.width), 1.0f / static_cast<float>(m_extent.height),
                                     CurrentWeight, history && m_historyValid ? 1.0f : 0.0f}};
        commands.BeginRenderPass({.colorTarget = m_history[target].get()});
        commands.SetViewport({0.0f, 0.0f, static_cast<float>(m_extent.width), static_cast<float>(m_extent.height)});
        commands.SetScissor({0, 0, m_extent.width, m_extent.height});
        commands.BindPipeline(*m_temporal);
        commands.SetBindGroup(0, *m_temporalGroups[target]);
        commands.PushConstants(&params, sizeof(params));
        commands.Draw(3, 0);
        commands.EndRenderPass();
        m_written = target;
        m_historyValid = true;
        m_output = 1 + target;
    }

    void PostRenderer::DrawBloom(RHI::CommandList& commands, float strength) {
        m_bloomDrawn = strength > 0.0f && m_levels > 0;
        if (!m_bloomDrawn) {
            return;
        }
        for (std::uint32_t i = 0; i < m_levels; ++i) {
            const RHI::Texture& source = i == 0 ? GetOutput(m_output) : *m_down[i - 1];
            DrawPass(commands, i == 0 ? *m_downsampleFirst : *m_downsample, *m_down[i],
                     i == 0 ? *m_firstDownGroups[m_output] : *m_downGroups[i], source, 0.0f);
        }
        for (std::uint32_t i = m_levels - 1; i-- > 0;) {
            const RHI::Texture& smaller = i + 2 < m_levels ? *m_up[i + 1] : *m_down[i + 1];
            DrawPass(commands, *m_upsample, *m_up[i], *m_upGroups[i], smaller, 1.0f);
        }
    }

    void PostRenderer::Composite(RHI::CommandList& commands, float strength, float exposure) const {
        commands.BindPipeline(*m_composite);
        commands.SetBindGroup(0, *m_compositeGroups[m_output]);
        const float levels = static_cast<float>(std::max(m_levels, 1u));
        const PostParams params{{}, {m_bloomDrawn ? strength : 0.0f, exposure, 1.0f / levels, 0.0f}};
        commands.PushConstants(&params, sizeof(params));
        commands.Draw(3, 0);
    }

    static_assert(sizeof(PostRenderer::GridParams) == 128, "les constantes poussées garanties partout");

    void PostRenderer::DrawGrid(RHI::CommandList& commands, const GridParams& params) const {
        commands.BindPipeline(*m_grid);
        commands.SetBindGroup(0, *m_gridGroup);
        commands.PushConstants(&params, sizeof(params));
        commands.Draw(3, 0);
    }

    Math::LinearColor PostRenderer::InverseToneMap(Math::LinearColor color, float exposure) {
        // ACES de Post.slang : c = x (2,51 x + 0,03) / (x (2,43 x + 0,59) + 0,14), résolue en x (racine positive).
        const auto inverse = [&](float c) {
            c = std::clamp(c, 0.0f, 1.0f);
            const float a = 2.51f - 2.43f * c;
            const float b = 0.03f - 0.59f * c;
            const float x = (-b + std::sqrt(b * b + 4.0f * a * 0.14f * c)) / (2.0f * a);
            return x / (std::max(exposure, 1.0e-4f) * 0.6f);
        };
        return {inverse(color.r), inverse(color.g), inverse(color.b), color.a};
    }
}
