#include "Renderer/ExposureRenderer.hpp"

#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <cmath>

namespace Flux {
    namespace {
        // Doit correspondre à Constants dans shaders/Exposure.slang.
        struct HistogramParams
        {
            std::array<float, 2> texel;
            std::array<std::uint32_t, 2> samples;
        };
        static_assert(sizeof(HistogramParams) == 16);

        constexpr std::uint32_t SceneSlot = 0;
        constexpr std::uint32_t BinsSlot = 1;
        constexpr std::uint32_t GroupSize = 16; // comme dans Exposure.slang
        constexpr std::uint64_t BinsSize = sizeof(Exposure::Histogram);
    }

    std::expected<std::unique_ptr<ExposureRenderer>, std::string> ExposureRenderer::Create(RHI::Device& device) {
        std::unique_ptr<ExposureRenderer> exposure(new ExposureRenderer(device));
        auto sampler = device.CreateSampler({.filter = RHI::Filter::Linear, .mipFilter = RHI::Filter::Nearest,
                                             .address = RHI::AddressMode::ClampToEdge});
        auto bins = device.CreateBuffer({BinsSize, RHI::BufferUsage::Storage, RHI::MemoryType::GpuOnly});
        if (!sampler || !bins) {
            return std::unexpected(!sampler ? sampler.error() : bins.error());
        }
        exposure->m_sampler = std::move(*sampler);
        exposure->m_bins = std::move(*bins);
        for (auto& readback : exposure->m_readback) {
            auto buffer = device.CreateBuffer({BinsSize, RHI::BufferUsage::Storage, RHI::MemoryType::Readback});
            if (!buffer) {
                return std::unexpected(buffer.error());
            }
            readback = std::move(*buffer);
        }

        constexpr std::array bindings{
            RHI::Binding{SceneSlot, RHI::BindingType::Texture, RHI::ShaderStage::Compute},
            RHI::Binding{BinsSlot, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Compute},
        };
        auto layout = device.CreateBindGroupLayout({.bindings = bindings, .maxGroups = 1});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        exposure->m_layout = std::move(*layout);
        exposure->m_group = device.CreateBindGroup(*exposure->m_layout);
        if (!exposure->m_group) {
            return std::unexpected("allocation du groupe de l'exposition");
        }
        exposure->m_group->SetBuffer(BinsSlot, *exposure->m_bins);

        const std::array<const RHI::BindGroupLayout*, 1> layouts{exposure->m_layout.get()};
        const auto shader = Platform::GetExecutableDirectory() / "shaders" / "Exposure.spv";
        auto clear = device.CreateComputePipeline({shader, "clearMain", layouts, sizeof(HistogramParams)});
        auto histogram = device.CreateComputePipeline({shader, "histogramMain", layouts, sizeof(HistogramParams)});
        if (!clear || !histogram) {
            return std::unexpected(!clear ? clear.error() : histogram.error());
        }
        exposure->m_clear = std::move(*clear);
        exposure->m_histogram = std::move(*histogram);
        return exposure;
    }

    void ExposureRenderer::Bind(const RHI::Texture& scene) {
        m_group->SetTexture(SceneSlot, scene, *m_sampler);
        m_extent = scene.GetExtent();
    }

    float ExposureRenderer::Update(const Exposure::Settings& settings, float seconds, bool snap, std::uint32_t frameIndex) {
        // Le GPU a fini l'image qui portait cet indice (BeginFrame l'a attendue) : sa copie est lisible.
        const bool measured = m_measured[frameIndex];
        m_measured[frameIndex] = false;
        if (!settings.automatic) {
            m_exposure = std::exp2(settings.compensation);
            m_luminance = 0.0f;
            m_adapted = false;
            return m_exposure;
        }
        if (snap) {
            m_adapted = false;
        }
        if (!measured) {
            return m_exposure;
        }
        m_readback[frameIndex]->Read(m_read.data(), BinsSize);
        std::optional<float> luminance = Exposure::MeteredLuminance(m_read);
        if (luminance) {
            *luminance /= m_preExposures[frameIndex];
        }
        m_luminance = luminance.value_or(0.0f);
        // Image noire : rien au-dessus de la plus basse case de l'histogramme, 7,5 stops sous le gris moyen (le fond de
        // l'espace après une planète éclairée). L'œil s'ouvre jusqu'à sa limite, et les étoiles apparaissent.
        const float target = luminance ? Exposure::Target(*luminance, settings) : std::exp2(settings.maximum);
        m_exposure = m_adapted ? Exposure::Adapt(m_exposure, target, seconds, settings) : target;
        // Une scène qui se charge commence souvent noire : la première vraie mesure est encore prise d'un coup.
        m_adapted = m_adapted || luminance.has_value();
        return m_exposure;
    }

    void ExposureRenderer::Measure(RHI::CommandList& commands, std::uint32_t frameIndex, float preExposure) {
        if (m_extent.width == 0 || m_extent.height == 0) {
            return;
        }
        const HistogramParams params{{1.0f / static_cast<float>(m_extent.width), 1.0f / static_cast<float>(m_extent.height)},
                                     {(m_extent.width + 1) / 2, (m_extent.height + 1) / 2}};
        commands.BindPipeline(*m_clear);
        commands.SetBindGroup(0, *m_group);
        commands.PushConstants(&params, sizeof(params));
        commands.Dispatch(1);
        commands.ComputeBarrier();
        commands.BindPipeline(*m_histogram);
        commands.SetBindGroup(0, *m_group);
        commands.PushConstants(&params, sizeof(params));
        commands.Dispatch((params.samples[0] + GroupSize - 1) / GroupSize, (params.samples[1] + GroupSize - 1) / GroupSize);
        // Lu par la copie (CopyBuffer pose sa barrière) ; la copie finie avant le calcul suivant qui le réécrit.
        commands.CopyBuffer(*m_bins, 0, *m_readback[frameIndex], 0, BinsSize);
        m_measured[frameIndex] = true;
        m_preExposures[frameIndex] = std::max(preExposure, 1.0e-6f);
    }
}
