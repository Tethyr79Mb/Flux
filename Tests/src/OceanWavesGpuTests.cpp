// Vagues de l'océan calculées par le GPU (Planetology::OceanWavesGpu) comparées à la somme directe, onde par onde, du
// même spectre (Flux/Planetology/OceanWaves.hpp) : hauteur, déplacement horizontal, pentes et dérivées en quelques
// points de chaque cascade, au temps donné. Vérifie la FFT (ordre, signes, décalage des indices) et le temps (vitesses
// arrondies). Aussi : écume entre 0 et 1, mip le plus petit à la moyenne (presque 0 pour la hauteur). Ignorés (réussis)
// sur une machine sans GPU Vulkan 1.3, sauf avec FLUX_TESTS_GPU=1 (la CI, sur lavapipe).
#include "Check.hpp"

#include "Planetology/OceanWavesGpu.hpp"
#include "Flux/Renderer/RHI/RHI.hpp"

#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using namespace Flux;
using namespace Flux::Planetology;

namespace {
    using Texel = std::array<float, 4>;

    // Doit correspondre à Constants dans Tests/shaders/OceanWavesTests.slang.
    struct Constants
    {
        std::uint32_t size;
        std::uint32_t level;
    };

    std::vector<Texel> ReadImage(RHI::Device& device, const RHI::Texture& image, std::uint32_t size, std::uint32_t level) {
        const std::uint32_t count = size * size;
        auto output = device.CreateBuffer({count * sizeof(Texel), RHI::BufferUsage::Storage, RHI::MemoryType::Readback});
        constexpr std::array bindings{RHI::Binding{0, RHI::BindingType::Texture, RHI::ShaderStage::Compute},
                                      RHI::Binding{1, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Compute}};
        auto layout = device.CreateBindGroupLayout({bindings, 1});
        auto sampler = device.CreateSampler({RHI::Filter::Nearest, RHI::Filter::Nearest});
        if (!output || !layout || !sampler) {
            return {};
        }
        const std::array<const RHI::BindGroupLayout*, 1> layouts{layout->get()};
        auto pipeline = device.CreateComputePipeline({"shaders/OceanWavesTests.spv", "computeMain", layouts, sizeof(Constants)});
        if (!pipeline) {
            std::printf("ÉCHEC %s\n", pipeline.error().c_str());
            return {};
        }
        std::vector<Texel> texels(count);
        {
            auto group = device.CreateBindGroup(**layout);
            group->SetTexture(0, image, **sampler);
            group->SetBuffer(1, **output);
            const Constants constants{size, level};
            const bool submitted = device.Submit([&](RHI::CommandList& commands) {
                commands.BindPipeline(**pipeline);
                commands.SetBindGroup(0, *group);
                commands.PushConstants(&constants, sizeof(constants));
                commands.Dispatch((count + 63) / 64);
                commands.ComputeBarrier(RHI::ComputeReaders::Host);
            });
            if (!submitted) {
                return {};
            }
        }
        (*output)->Read(texels.data(), count * sizeof(Texel));
        return texels;
    }

    // Les huit valeurs d'un point de l'eau par la somme directe, dans l'ordre des textures : déplacement (x, hauteur,
    // z), puis pentes (d hauteur / dx, d hauteur / dz, dx / dx, dz / dz) ; déplacements horizontaux x choppiness.
    std::array<double, 7> DirectSum(const OceanSpectrum& spectrum, std::uint32_t cascade, double x, double z, double time,
                                    double lambda) {
        std::array<double, 7> sum{};
        constexpr std::uint32_t N = OceanWaveSize;
        for (std::uint32_t n = 0; n < N; ++n) {
            for (std::uint32_t m = 0; m < N; ++m) {
                const std::array<float, 4>& s = spectrum[n * N + m];
                if (s[0] == 0.0f && s[1] == 0.0f && s[2] == 0.0f && s[3] == 0.0f) {
                    continue;
                }
                const auto [kx, kz] = OceanWaveVector(cascade, m, n);
                const double k = std::sqrt(double{kx} * kx + double{kz} * kz);
                const double phase = OceanAngularSpeed(static_cast<float>(k)) * time;
                const std::complex<double> turn = std::polar(1.0, phase);
                const std::complex<double> h = std::complex<double>(s[0], s[1]) * std::conj(turn)
                                             + std::complex<double>(s[2], s[3]) * turn;
                const std::complex<double> wave = h * std::polar(1.0, kx * x + kz * z);
                const std::complex<double> i(0.0, 1.0);
                sum[0] += lambda * (-i * (kx / k) * wave).real();
                sum[1] += wave.real();
                sum[2] += lambda * (-i * (kz / k) * wave).real();
                sum[3] += (i * double{kx} * wave).real();
                sum[4] += (i * double{kz} * wave).real();
                sum[5] += lambda * (kx * kx / k * wave).real();
                sum[6] += lambda * (kz * kz / k * wave).real();
            }
        }
        return sum;
    }
}

bool RunOceanWavesGpuTests() {
    auto device = RHI::Device::CreateHeadless();
    if (!device) {
        return GpuMissing("vagues de l'océan", device.error().c_str());
    }
    auto waves = OceanWavesGpu::Create(**device);
    if (!waves) {
        std::printf("ÉCHEC %s\n", waves.error().c_str());
        return false;
    }
    OceanWaveSettings settings;
    settings.windDirection = 30.0f;
    settings.seed = 3;
    CHECK((*waves)->SetSettings(settings));
    // Deux images : la seconde relit l'écume de la première. Temps au-delà de la période : ramené dedans.
    constexpr double Time = OceanRepeatPeriod + 3.7;
    CHECK((*device)->Submit([&](RHI::CommandList& commands) { (*waves)->Update(commands, Time - 0.02, 0.02f); }));
    CHECK((*device)->Submit([&](RHI::CommandList& commands) { (*waves)->Update(commands, Time, 0.02f); }));

    constexpr std::uint32_t N = OceanWaveSize;
    constexpr std::array<std::array<std::uint32_t, 2>, 5> Points{{{0, 0}, {3, 7}, {100, 200}, {255, 1}, {128, 129}}};
    for (std::uint32_t cascade = 0; cascade < OceanCascadeCount; ++cascade) {
        const std::vector<Texel> displacement = ReadImage(**device, (*waves)->GetDisplacement(cascade), N, 0);
        const std::vector<Texel> slopes = ReadImage(**device, (*waves)->GetSlopes(cascade), N, 0);
        CHECK(displacement.size() == std::size_t{N} * N && slopes.size() == displacement.size());
        // Écart type de chaque valeur sur la cascade : l'échelle des écarts permis (16 bits flottants, sommes en float).
        std::array<double, 7> deviation{};
        double meanHeight = 0.0;
        for (std::size_t i = 0; i < displacement.size(); ++i) {
            const std::array<float, 7> values{displacement[i][0], displacement[i][1], displacement[i][2], slopes[i][0],
                                              slopes[i][1], slopes[i][2], slopes[i][3]};
            for (std::size_t v = 0; v < values.size(); ++v) {
                deviation[v] += double{values[v]} * values[v];
            }
            meanHeight += displacement[i][1];
            CHECK(displacement[i][3] >= 0.0f && displacement[i][3] <= 1.0f);
        }
        for (double& d : deviation) {
            d = std::sqrt(d / static_cast<double>(displacement.size()));
        }
        meanHeight /= static_cast<double>(displacement.size());
        std::printf("       cascade %u : hauteur écart type %.3f m, moyenne %.1e m, pente %.3f\n", cascade, deviation[1],
                    meanHeight, deviation[3]);
        CHECK(deviation[1] > 0.0);
        CHECK(std::abs(meanHeight) < 0.01 * deviation[1] + 1e-4);

        const OceanSpectrum spectrum = BuildOceanSpectrum(settings, cascade);
        const double step = OceanCascadeLengths[cascade] / N;
        const double time = std::fmod(Time, double{OceanRepeatPeriod});
        double worst = 0.0;
        for (const auto& [x, z] : Points) {
            const std::array<double, 7> expected = DirectSum(spectrum, cascade, x * step, z * step, time, settings.choppiness);
            const Texel& d = displacement[z * N + x];
            const Texel& s = slopes[z * N + x];
            const std::array<float, 7> got{d[0], d[1], d[2], s[0], s[1], s[2], s[3]};
            for (std::size_t v = 0; v < got.size(); ++v) {
                const double error = std::abs(got[v] - expected[v]) / std::max(deviation[v], 1e-6);
                worst = std::max(worst, error);
                if (error > 0.01) {
                    std::printf("       cascade %u (%u, %u), valeur %zu : GPU %.5f, somme directe %.5f\n", cascade, x, z, v,
                                got[v], expected[v]);
                }
            }
        }
        std::printf("       cascade %u : plus grand écart à la somme directe %.4f écart type\n", cascade, worst);
        CHECK(worst < 0.01);

        // Mip de 1 pixel : la moyenne de la cascade.
        const std::vector<Texel> last = ReadImage(**device, (*waves)->GetDisplacement(cascade), 1, 8);
        CHECK(last.size() == 1);
        CHECK(std::abs(last[0][1] - meanHeight) < 0.01 * deviation[1] + 1e-3);
    }
    return true;
}
