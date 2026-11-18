#include "Renderer/AmbientOcclusionRenderer.hpp"

#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace Flux {
    namespace {
        // Doit correspondre à AmbientOcclusionParams dans shaders/AmbientOcclusion.slang.
        struct AmbientOcclusionParams
        {
            std::array<float, 4> right;
            std::array<float, 4> up;
            std::array<float, 4> forward;
            std::array<float, 4> size;     // largeur, hauteur, mètres par pixel à 1 m, rayon
            std::array<float, 4> settings; // atténuation (multiplicateur, ajout), puissance, rayon max (pixels)
            std::array<std::uint32_t, 4> options; // image (bruit), pas entre deux pixels, tranches, pas par côté
        };
        static_assert(sizeof(AmbientOcclusionParams) == 96);

        constexpr std::uint32_t DepthSlot = 0;
        constexpr std::uint32_t BaseSlot = 1;
        constexpr std::uint32_t NormalSlot = 2;
        constexpr std::uint32_t TargetSlot = 3;
        constexpr std::uint32_t SourceSlot = 4;
        constexpr std::uint32_t GroupSize = 8; // comme dans AmbientOcclusion.slang
        // Puissance de la sortie à l'intensité 1 (XeGTAO : FinalValuePower).
        constexpr float BasePower = 2.2f;
        // Rayon le plus grand à l'écran, en pixels de rendu (voir la classe).
        constexpr float MaxScreenRadius = 128.0f;

        struct QualitySteps
        {
            std::uint32_t stride; // 1 : chaque pixel ; 2 : un sur quatre
            std::uint32_t slices;
            std::uint32_t steps;
        };
        constexpr QualitySteps GetSteps(AmbientOcclusionQuality quality) {
            // Haute : le « High » de XeGTAO ; Moyenne : son « Medium » sur un quart des pixels.
            return quality == AmbientOcclusionQuality::High ? QualitySteps{1, 3, 3} : QualitySteps{2, 2, 3};
        }
    }

    std::expected<std::unique_ptr<AmbientOcclusionRenderer>, std::string> AmbientOcclusionRenderer::Create(RHI::Device& device) {
        std::unique_ptr<AmbientOcclusionRenderer> renderer(new AmbientOcclusionRenderer(device));
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
            RHI::Binding{TargetSlot, RHI::BindingType::StorageTexture, compute},
            RHI::Binding{SourceSlot, RHI::BindingType::Texture, compute},
        };
        auto layout = device.CreateBindGroupLayout({.bindings = bindings, .maxGroups = 2});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        renderer->m_layout = std::move(*layout);
        renderer->m_computeGroup = device.CreateBindGroup(*renderer->m_layout);
        renderer->m_denoiseGroup = device.CreateBindGroup(*renderer->m_layout);
        if (!renderer->m_computeGroup || !renderer->m_denoiseGroup) {
            return std::unexpected("allocation des groupes de l'occlusion ambiante");
        }

        const std::array<const RHI::BindGroupLayout*, 1> layouts{renderer->m_layout.get()};
        const auto shader = Platform::GetExecutableDirectory() / "shaders" / "AmbientOcclusion.spv";
        auto ambient = device.CreateComputePipeline({shader, "computeAmbientOcclusion", layouts, sizeof(AmbientOcclusionParams)});
        auto denoise = device.CreateComputePipeline({shader, "computeDenoise", layouts, sizeof(AmbientOcclusionParams)});
        if (!ambient || !denoise) {
            return std::unexpected(!ambient ? ambient.error() : denoise.error());
        }
        renderer->m_compute = std::move(*ambient);
        renderer->m_denoise = std::move(*denoise);
        return renderer;
    }

    void AmbientOcclusionRenderer::Bind(const Targets& targets) {
        m_extent = targets.depth->GetExtent();
        m_computeGroup->SetTexture(DepthSlot, *targets.depth, *m_pointSampler);
        m_computeGroup->SetTexture(BaseSlot, *targets.base, *m_pointSampler);
        m_computeGroup->SetTexture(NormalSlot, *targets.normals, *m_pointSampler);
        m_computeGroup->SetStorageTexture(TargetSlot, *targets.raw);
        m_denoiseGroup->SetTexture(SourceSlot, *targets.raw, *m_pointSampler);
        m_denoiseGroup->SetStorageTexture(TargetSlot, *targets.filtered);
    }

    std::uint32_t AmbientOcclusionRenderer::GetStride() const {
        return IsActive() ? GetSteps(m_quality).stride : 0u;
    }

    void AmbientOcclusionRenderer::Run(RHI::CommandList& commands, const RHI::Pipeline& pipeline, const RHI::BindGroup& group,
                                       const MeshRenderer::RayShadowView& view, const Settings& settings) const {
        const QualitySteps steps = GetSteps(m_quality);
        // Hauteur du champ à 1 m (deux fois la longueur de `up`) sur la hauteur de l'image.
        const float upLength = std::sqrt(view.up[0] * view.up[0] + view.up[1] * view.up[1] + view.up[2] * view.up[2]);
        const float radius = std::max(settings.radius, 0.01f);
        // Poids d'un obstacle : 1 jusqu'au début du fondu, 0 au rayon (XeGTAO : FalloffRange).
        const float falloffRange = std::clamp(settings.falloff, 0.01f, 1.0f) * radius;
        const float falloffFrom = radius - falloffRange;
        const AmbientOcclusionParams params{
            view.right, view.up, view.forward,
            {static_cast<float>(m_extent.width), static_cast<float>(m_extent.height),
             2.0f * upLength / static_cast<float>(std::max(m_extent.height, 1u)), radius},
            {-1.0f / falloffRange, falloffFrom / falloffRange + 1.0f, BasePower * std::max(settings.intensity, 0.0f), MaxScreenRadius},
            {view.noiseFrame, steps.stride, steps.slices, steps.steps}};
        // Moyenne : la grille des pixels calculés (le quart en haut à gauche des textures).
        const std::uint32_t width = (m_extent.width + steps.stride - 1) / steps.stride;
        const std::uint32_t height = (m_extent.height + steps.stride - 1) / steps.stride;
        commands.BindPipeline(pipeline);
        commands.SetBindGroup(0, group);
        commands.PushConstants(&params, sizeof(params));
        commands.Dispatch((width + GroupSize - 1) / GroupSize, (height + GroupSize - 1) / GroupSize);
    }

    void AmbientOcclusionRenderer::Compute(RHI::CommandList& commands, const MeshRenderer::RayShadowView& view,
                                           const Settings& settings) const {
        Run(commands, *m_compute, *m_computeGroup, view, settings);
    }

    void AmbientOcclusionRenderer::Denoise(RHI::CommandList& commands, const MeshRenderer::RayShadowView& view,
                                           const Settings& settings) const {
        Run(commands, *m_denoise, *m_denoiseGroup, view, settings);
    }
}
