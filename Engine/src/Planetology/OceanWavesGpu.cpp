#include "Planetology/OceanWavesGpu.hpp"

#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

namespace Flux::Planetology {
    namespace {
        // Doit correspondre à Constants dans shaders/OceanWaves.slang.
        struct Constants
        {
            std::uint32_t cascade;
            std::uint32_t columns;
            float time;
            float length;
            float choppiness;
            float foamKeep;
            float foamStart;
            float foamFull;
        };
        static_assert(sizeof(Constants) == 32);

        constexpr std::uint32_t N = OceanWaveSize;
        constexpr std::uint32_t Fields = 4;     // comme dans OceanWaves.slang
        constexpr std::uint32_t GroupSide = 8;  // numthreads (8, 8) du spectre et du rangement
        constexpr std::uint64_t SpectrumBytes = std::uint64_t{N} * N * 16; // par cascade
        constexpr std::uint64_t FieldBytes = std::uint64_t{N} * N * 8 * Fields;
        constexpr std::uint32_t MipLevels = static_cast<std::uint32_t>(std::bit_width(N));
        // Écume : naît sous ce jacobien (surface resserrée), pleine sous le second ; s'efface en quelques secondes.
        constexpr float FoamStart = 0.5f;
        constexpr float FoamFull = 0.1f;
        constexpr float FoamLifetime = 2.5f;
    }

    std::expected<std::unique_ptr<OceanWavesGpu>, std::string> OceanWavesGpu::Create(RHI::Device& device, std::uint32_t cascades) {
        std::unique_ptr<OceanWavesGpu> waves(new OceanWavesGpu(device));
        waves->m_cascades = std::clamp(cascades, 1u, OceanCascadeCount);
        auto spectrum = device.CreateBuffer({SpectrumBytes * waves->m_cascades, RHI::BufferUsage::Storage, RHI::MemoryType::GpuOnly});
        auto fields = device.CreateBuffer({FieldBytes * waves->m_cascades, RHI::BufferUsage::Storage, RHI::MemoryType::GpuOnly});
        if (!spectrum || !fields) {
            return std::unexpected(!spectrum ? spectrum.error() : fields.error());
        }
        waves->m_spectrum = std::move(*spectrum);
        waves->m_fields = std::move(*fields);

        constexpr std::array bindings{
            RHI::Binding{0, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Compute},
            RHI::Binding{1, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Compute},
            RHI::Binding{2, RHI::BindingType::StorageTexture, RHI::ShaderStage::Compute},
            RHI::Binding{3, RHI::BindingType::StorageTexture, RHI::ShaderStage::Compute},
        };
        auto layout = device.CreateBindGroupLayout({bindings, OceanCascadeCount});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        waves->m_layout = std::move(*layout);
        for (std::uint32_t cascade = 0; cascade < waves->m_cascades; ++cascade) {
            auto displacement = device.CreateRenderTarget(RHI::Format::RGBA16Float, {N, N}, true, true, MipLevels);
            auto slopes = device.CreateRenderTarget(RHI::Format::RGBA16Float, {N, N}, true, true, MipLevels);
            if (!displacement || !slopes) {
                return std::unexpected(!displacement ? displacement.error() : slopes.error());
            }
            waves->m_displacement[cascade] = std::move(*displacement);
            waves->m_slopes[cascade] = std::move(*slopes);
            auto group = device.CreateBindGroup(*waves->m_layout);
            if (!group) {
                return std::unexpected("vagues de l'océan : groupes de ressources");
            }
            group->SetBuffer(0, *waves->m_spectrum);
            group->SetBuffer(1, *waves->m_fields);
            group->SetStorageTexture(2, *waves->m_displacement[cascade]);
            group->SetStorageTexture(3, *waves->m_slopes[cascade]);
            waves->m_groups[cascade] = std::move(group);
        }

        const std::array<const RHI::BindGroupLayout*, 1> layouts{waves->m_layout.get()};
        const auto shader = Platform::GetExecutableDirectory() / "shaders" / "OceanWaves.spv";
        auto spectrumPipeline = device.CreateComputePipeline({shader, "computeSpectrum", layouts, sizeof(Constants)});
        auto fftPipeline = device.CreateComputePipeline({shader, "computeFft", layouts, sizeof(Constants)});
        auto assemblePipeline = device.CreateComputePipeline({shader, "computeAssemble", layouts, sizeof(Constants)});
        if (!spectrumPipeline || !fftPipeline || !assemblePipeline) {
            return std::unexpected(!spectrumPipeline ? spectrumPipeline.error()
                                   : !fftPipeline    ? fftPipeline.error()
                                                     : assemblePipeline.error());
        }
        waves->m_spectrumPipeline = std::move(*spectrumPipeline);
        waves->m_fftPipeline = std::move(*fftPipeline);
        waves->m_assemblePipeline = std::move(*assemblePipeline);
        return waves;
    }

    bool OceanWavesGpu::SetSettings(const OceanWaveSettings& settings) {
        if (m_hasSettings && settings == m_settings) {
            return true;
        }
        std::vector<std::array<float, 4>> data;
        data.reserve(std::size_t{N} * N * m_cascades);
        for (std::uint32_t cascade = 0; cascade < m_cascades; ++cascade) {
            const OceanSpectrum spectrum = BuildOceanSpectrum(settings, cascade);
            data.insert(data.end(), spectrum.begin(), spectrum.end());
        }
        if (!m_device.WriteBuffer(*m_spectrum, 0, data.data(), data.size() * sizeof(data[0]))) {
            return false;
        }
        m_settings = settings;
        m_hasSettings = true;
        return true;
    }

    void OceanWavesGpu::Update(RHI::CommandList& commands, double time, float seconds) {
        if (!m_hasSettings) {
            return;
        }
        Constants constants{};
        constants.time = static_cast<float>(std::fmod(std::max(time, 0.0), double{OceanRepeatPeriod}));
        constants.choppiness = m_settings.choppiness;
        constants.foamKeep = m_hasFoam ? std::exp(-std::max(seconds, 0.0f) / FoamLifetime) : 0.0f;
        constants.foamStart = FoamStart;
        constants.foamFull = FoamFull;
        const auto run = [&](const RHI::Pipeline& pipeline, std::uint32_t columns, std::uint32_t groupsX, std::uint32_t groupsY) {
            commands.BindPipeline(pipeline);
            for (std::uint32_t cascade = 0; cascade < m_cascades; ++cascade) {
                constants.cascade = cascade;
                constants.columns = columns;
                constants.length = OceanCascadeLengths[cascade];
                commands.SetBindGroup(0, *m_groups[cascade]);
                commands.PushConstants(&constants, sizeof(constants));
                commands.Dispatch(groupsX, groupsY);
            }
            commands.ComputeBarrier(RHI::ComputeReaders::Compute);
        };
        run(*m_spectrumPipeline, 0, N / GroupSide, N / GroupSide);
        run(*m_fftPipeline, 0, N * Fields, 1); // un groupe par ligne de chaque champ
        run(*m_fftPipeline, 1, N * Fields, 1); // puis par colonne
        std::array<RHI::TextureUse, OceanCascadeCount * 2> uses{};
        for (std::uint32_t cascade = 0; cascade < m_cascades; ++cascade) {
            // Le déplacement garde son écume ; les pentes sont réécrites en entier.
            uses[cascade * 2] = {m_displacement[cascade].get(), RHI::TextureAccess::Storage, !m_hasFoam};
            uses[cascade * 2 + 1] = {m_slopes[cascade].get(), RHI::TextureAccess::Storage, true};
        }
        commands.UseTextures({uses.data(), m_cascades * 2});
        run(*m_assemblePipeline, 0, N / GroupSide, N / GroupSide);
        for (std::uint32_t cascade = 0; cascade < m_cascades; ++cascade) {
            commands.GenerateMipmaps(*m_displacement[cascade]);
            commands.GenerateMipmaps(*m_slopes[cascade]);
        }
        m_hasFoam = true;
    }
}
