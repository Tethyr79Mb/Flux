#include "Renderer/EnvironmentRenderer.hpp"

#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace Flux {
    namespace {
        // Doit correspondre à EnvironmentParams dans shaders/Environment.slang.
        struct EnvironmentParams
        {
            std::array<float, 4> top;
            std::array<float, 4> bottom;
            std::uint32_t face = 0;
            std::uint32_t mode = 0;
            float roughness = 0.0f;
            float sourceSize = 0.0f;
        };
        static_assert(sizeof(EnvironmentParams) == 48);

        constexpr std::uint32_t PanoramaSlot = 0;
        constexpr std::uint32_t BackgroundSlot = 1;
        constexpr std::uint32_t SkySlot = 0;

        // Le ciel et ses reflets : 128 pixels par face suffisent aux reflets flous des surfaces rugueuses ; un reflet
        // net (verre, métal poli) aura besoin des reflets de l'écran ou de sondes (étapes suivantes, Engine/ROADMAP.md).
        constexpr std::uint32_t SkySize = 128;
        // Rugosité 0, 0,2, 0,4... 1 : 6 niveaux, de 128 à 4 pixels. Mesh.slang lit le niveau rugosité x 5.
        constexpr std::uint32_t ReflectionMips = 6;
        // Lumière diffuse : très lisse d'une direction à l'autre, 16 pixels par face suffisent.
        constexpr std::uint32_t IrradianceSize = 16;
    }

    std::expected<std::unique_ptr<EnvironmentRenderer>, std::string> EnvironmentRenderer::Create(RHI::Device& device) {
        std::unique_ptr<EnvironmentRenderer> environment(new EnvironmentRenderer(device));
        auto sampler = device.CreateSampler({.filter = RHI::Filter::Linear, .mipFilter = RHI::Filter::Linear,
                                             .address = RHI::AddressMode::ClampToEdge});
        auto panoramaSampler = device.CreateSampler({.filter = RHI::Filter::Linear, .mipFilter = RHI::Filter::Linear,
                                                     .address = RHI::AddressMode::Repeat});
        if (!sampler || !panoramaSampler) {
            return std::unexpected(!sampler ? sampler.error() : panoramaSampler.error());
        }
        environment->m_sampler = std::move(*sampler);
        environment->m_panoramaSampler = std::move(*panoramaSampler);

        constexpr std::array<std::uint8_t, 4> black{0, 0, 0, 255};
        constexpr std::array<std::uint8_t, 4 * 6> blackFaces{0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255,
                                                             0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};
        auto blackTexture = device.CreateTexture(RHI::Format::RGBA8Srgb, {1, 1}, black);
        auto blackCube = device.CreateCubeTexture(RHI::Format::RGBA8Unorm, 1, blackFaces);
        auto sky = device.CreateCubeRenderTarget(Format, SkySize, static_cast<std::uint32_t>(std::bit_width(SkySize)));
        auto reflections = device.CreateCubeRenderTarget(Format, SkySize, ReflectionMips);
        auto irradiance = device.CreateCubeRenderTarget(Format, IrradianceSize);
        if (!blackTexture || !blackCube || !sky || !reflections || !irradiance) {
            return std::unexpected("création des textures de l'environnement");
        }
        environment->m_black = std::move(*blackTexture);
        environment->m_blackCube = std::move(*blackCube);
        environment->m_sky = std::move(*sky);
        environment->m_reflections = std::move(*reflections);
        environment->m_irradiance = std::move(*irradiance);

        constexpr std::array inputBindings{
            RHI::Binding{PanoramaSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
            RHI::Binding{BackgroundSlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment},
        };
        constexpr std::array skyBindings{RHI::Binding{SkySlot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment}};
        auto inputLayout = device.CreateBindGroupLayout({.bindings = inputBindings, .maxGroups = RHI::FramesInFlight});
        auto skyLayout = device.CreateBindGroupLayout({.bindings = skyBindings, .maxGroups = 1});
        if (!inputLayout || !skyLayout) {
            return std::unexpected(!inputLayout ? inputLayout.error() : skyLayout.error());
        }
        environment->m_inputLayout = std::move(*inputLayout);
        environment->m_skyLayout = std::move(*skyLayout);
        for (std::uint32_t f = 0; f < RHI::FramesInFlight; ++f) {
            environment->m_inputGroups[f] = device.CreateBindGroup(*environment->m_inputLayout);
            if (!environment->m_inputGroups[f]) {
                return std::unexpected("allocation des groupes de l'environnement");
            }
            environment->m_inputGroups[f]->SetTexture(PanoramaSlot, *environment->m_black, *environment->m_panoramaSampler);
            environment->m_inputGroups[f]->SetTexture(BackgroundSlot, *environment->m_blackCube, *environment->m_sampler);
        }
        environment->m_skyGroup = device.CreateBindGroup(*environment->m_skyLayout);
        if (!environment->m_skyGroup) {
            return std::unexpected("allocation des groupes de l'environnement");
        }
        environment->m_skyGroup->SetTexture(SkySlot, *environment->m_sky, *environment->m_sampler);

        const RHI::BindGroupLayout* sourceLayouts[] = {environment->m_inputLayout.get()};
        const RHI::BindGroupLayout* filterLayouts[] = {environment->m_inputLayout.get(), environment->m_skyLayout.get()};
        RHI::PipelineDesc desc;
        desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Environment.spv";
        desc.pushConstantsSize = sizeof(EnvironmentParams);
        desc.colorFormat = Format;
        desc.fragmentEntryPoint = "sourceFragment";
        desc.bindGroupLayouts = sourceLayouts;
        auto sourcePipeline = device.CreatePipeline(desc);
        desc.bindGroupLayouts = filterLayouts;
        desc.fragmentEntryPoint = "prefilterFragment";
        auto prefilterPipeline = device.CreatePipeline(desc);
        desc.fragmentEntryPoint = "irradianceFragment";
        auto irradiancePipeline = device.CreatePipeline(desc);
        if (!sourcePipeline || !prefilterPipeline || !irradiancePipeline) {
            return std::unexpected(!sourcePipeline      ? sourcePipeline.error()
                                   : !prefilterPipeline ? prefilterPipeline.error()
                                                        : irradiancePipeline.error());
        }
        environment->m_sourcePipeline = std::move(*sourcePipeline);
        environment->m_prefilterPipeline = std::move(*prefilterPipeline);
        environment->m_irradiancePipeline = std::move(*irradiancePipeline);
        return environment;
    }

    std::uint32_t EnvironmentRenderer::GetReflectionMips() const {
        return ReflectionMips;
    }

    void EnvironmentRenderer::Prepare(const EnvironmentSource& source, std::uint32_t frameIndex) {
        if (!m_baked || source != m_source) {
            m_source = source;
            m_pending = true;
        }
        // Le groupe de cette image : le GPU a fini de lire celui-ci (après BeginFrame).
        EnvironmentSource& bound = m_boundInputs[frameIndex];
        if (bound.panorama != source.panorama) {
            m_inputGroups[frameIndex]->SetTexture(PanoramaSlot, source.panorama ? *source.panorama : *m_black, *m_panoramaSampler);
            bound.panorama = source.panorama;
        }
        if (bound.background != source.background) {
            m_inputGroups[frameIndex]->SetTexture(BackgroundSlot, source.background ? *source.background : *m_blackCube, *m_sampler);
            bound.background = source.background;
        }
    }

    void EnvironmentRenderer::Bake(RHI::CommandList& commands, std::uint32_t frameIndex) {
        m_pending = false;
        m_baked = true;
        EnvironmentParams params;
        params.top = {m_source.top.r, m_source.top.g, m_source.top.b, 0.0f};
        params.bottom = {m_source.bottom.r, m_source.bottom.g, m_source.bottom.b, 0.0f};
        params.mode = static_cast<std::uint32_t>(m_source.kind);
        params.sourceSize = static_cast<float>(SkySize);
        if (m_source.kind == EnvironmentSource::Kind::Generated && m_source.background) {
            params.top[3] = 2.0f / static_cast<float>(m_source.background->GetExtent().width);
        }
        if (m_source.kind == EnvironmentSource::Kind::Panorama && m_source.panorama) {
            // Un point sur 4 x 4 par texel : un quart de texel du cube, soit pi / (2 x 128 x 4) ; un pixel du panorama
            // couvre 2 pi / largeur.
            const float width = static_cast<float>(m_source.panorama->GetExtent().width);
            params.bottom[3] = std::max(std::log2(width / (4.0f * 4.0f * static_cast<float>(SkySize))), 0.0f);
        }
        const auto face = [&](const RHI::Texture& target, std::uint32_t layer, std::uint32_t mip, const RHI::Pipeline& pipeline) {
            const std::uint32_t size = std::max(target.GetExtent().width >> mip, 1u);
            commands.BeginRenderPass({.clearColor = {0.0f, 0.0f, 0.0f, 1.0f}, .colorTarget = &target, .colorLayer = layer,
                                      .colorMip = mip});
            commands.SetViewport({0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size)});
            commands.SetScissor({0, 0, size, size});
            commands.BindPipeline(pipeline);
            commands.SetBindGroup(0, *m_inputGroups[frameIndex]);
            if (&pipeline != m_sourcePipeline.get()) {
                commands.SetBindGroup(1, *m_skyGroup);
            }
            params.face = layer;
            commands.PushConstants(&params, sizeof(params));
            commands.Draw(3, 0);
            commands.EndRenderPass();
        };

        // 1) Le ciel en HDR, puis ses mips : chaque tirage des étapes suivantes lit le niveau qui couvre son angle.
        commands.Use(*m_sky, RHI::TextureAccess::ColorTarget, true);
        for (std::uint32_t f = 0; f < 6; ++f) {
            face(*m_sky, f, 0, *m_sourcePipeline);
        }
        commands.GenerateMipmaps(*m_sky); // le laisse lisible
        // 2) Reflets, du miroir au plus rugueux.
        for (std::uint32_t mip = 0; mip < ReflectionMips; ++mip) {
            params.roughness = static_cast<float>(mip) / static_cast<float>(ReflectionMips - 1);
            for (std::uint32_t f = 0; f < 6; ++f) {
                face(*m_reflections, f, mip, *m_prefilterPipeline);
            }
        }
        // 3) Lumière diffuse.
        for (std::uint32_t f = 0; f < 6; ++f) {
            face(*m_irradiance, f, 0, *m_irradiancePipeline);
        }
    }
}
