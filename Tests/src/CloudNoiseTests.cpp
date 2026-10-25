// Bruit 3D des nuages calculé par le GPU (Planetology::CloudNoise) relu pixel par pixel : valeurs de 0 à 1 qui varient,
// textures qui se répètent sans couture (d'un bord à l'autre, l'écart est celui de deux voisins), mips qui gardent la
// moyenne. Passe aussi par les textures 3D du RHI (écriture par compute shader, GenerateMipmaps). Ignorés (réussis)
// sur une machine sans GPU Vulkan 1.3, sauf avec FLUX_TESTS_GPU=1 (la CI, sur lavapipe).
#include "Check.hpp"

#include "Planetology/CloudNoise.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace Flux;

namespace {
    using Texel = std::array<float, 4>; // r, g, b, a

    // Doit correspondre à Constants dans Tests/shaders/CloudNoiseTests.slang.
    struct Constants
    {
        std::uint32_t size;
        std::uint32_t level;
    };

    // Le niveau `level` (côté `size`) de la texture, dans l'ordre x, y, z ; vide en cas d'échec.
    std::vector<Texel> ReadVolume(RHI::Device& device, const RHI::Texture& volume, std::uint32_t size, std::uint32_t level) {
        const std::uint32_t count = size * size * size;
        auto output = device.CreateBuffer({count * sizeof(Texel), RHI::BufferUsage::Storage, RHI::MemoryType::Readback});
        constexpr std::array bindings{RHI::Binding{0, RHI::BindingType::Texture, RHI::ShaderStage::Compute},
                                      RHI::Binding{1, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Compute}};
        auto layout = device.CreateBindGroupLayout({bindings, 1});
        auto sampler = device.CreateSampler({RHI::Filter::Nearest, RHI::Filter::Nearest});
        if (!output || !layout || !sampler) {
            return {};
        }
        const std::array<const RHI::BindGroupLayout*, 1> layouts{layout->get()};
        auto pipeline = device.CreateComputePipeline({"shaders/CloudNoiseTests.spv", "computeMain", layouts, sizeof(Constants)});
        if (!pipeline) {
            std::printf("ÉCHEC %s\n", pipeline.error().c_str());
            return {};
        }
        std::vector<Texel> texels(count);
        {
            auto group = device.CreateBindGroup(**layout);
            group->SetTexture(0, volume, **sampler);
            group->SetBuffer(1, **output);
            const Constants constants{size, level};
            const bool submitted = device.Submit([&](RHI::CommandList& commands) {
                commands.BindPipeline(**pipeline);
                commands.SetBindGroup(0, *group);
                commands.PushConstants(&constants, sizeof(constants));
                commands.Dispatch((count + 63) / 64);
                commands.ComputeBarrier();
            });
            if (!submitted) {
                return {};
            }
        }
        (*output)->Read(texels.data(), count * sizeof(Texel));
        return texels;
    }

    // Une texture de bruit : chaque canal utilisé (`channels` premiers) de 0 à 1, varié, et sans couture.
    bool CheckVolume(const std::vector<Texel>& texels, std::uint32_t size, std::size_t channels, const char* name) {
        CHECK(texels.size() == std::size_t{size} * size * size);
        const auto at = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z) -> const Texel& {
            return texels[(std::size_t{z} * size + y) * size + x];
        };
        for (std::size_t channel = 0; channel < channels; ++channel) {
            double sum = 0.0;
            double squares = 0.0;
            for (const Texel& texel : texels) {
                CHECK(texel[channel] >= 0.0f && texel[channel] <= 1.0f);
                sum += texel[channel];
                squares += double{texel[channel]} * texel[channel];
            }
            const double mean = sum / static_cast<double>(texels.size());
            const double deviation = std::sqrt(std::max(squares / static_cast<double>(texels.size()) - mean * mean, 0.0));
            // Écart moyen entre voisins dans la texture, et du dernier pixel au premier (la couture), sur les trois axes.
            double inside = 0.0;
            double seam = 0.0;
            for (std::uint32_t a = 0; a < size; ++a) {
                for (std::uint32_t b = 0; b < size; ++b) {
                    inside += std::abs(at(size / 2, a, b)[channel] - at(size / 2 - 1, a, b)[channel])
                            + std::abs(at(a, size / 2, b)[channel] - at(a, size / 2 - 1, b)[channel])
                            + std::abs(at(a, b, size / 2)[channel] - at(a, b, size / 2 - 1)[channel]);
                    seam += std::abs(at(0, a, b)[channel] - at(size - 1, a, b)[channel])
                          + std::abs(at(a, 0, b)[channel] - at(a, size - 1, b)[channel])
                          + std::abs(at(a, b, 0)[channel] - at(a, b, size - 1)[channel]);
                }
            }
            std::printf("       bruit %s, canal %zu : moyenne %.3f, écart type %.3f, voisins %.4f, couture %.4f\n", name, channel,
                        mean, deviation, inside / (3.0 * size * size), seam / (3.0 * size * size));
            CHECK(deviation > 0.05);
            // Sans répétition, la couture vaudrait l'écart de deux valeurs au hasard, bien plus que celui de deux voisins.
            CHECK(seam < inside * 1.5 + 0.002 * size * size);
        }
        return true;
    }

    double Mean(const std::vector<Texel>& texels, std::size_t channel) {
        double sum = 0.0;
        for (const Texel& texel : texels) {
            sum += texel[channel];
        }
        return sum / static_cast<double>(texels.size());
    }
}

bool RunCloudNoiseTests() {
    auto device = RHI::Device::CreateHeadless();
    if (!device) {
        return GpuMissing("bruit des nuages", device.error().c_str());
    }
    auto noise = Planetology::CloudNoise::Create(**device);
    if (!noise) {
        std::printf("ÉCHEC %s\n", noise.error().c_str());
        return false;
    }
    constexpr std::uint32_t Shape = Planetology::CloudNoise::ShapeSize;
    constexpr std::uint32_t Detail = Planetology::CloudNoise::DetailSize;
    const std::vector<Texel> shape = ReadVolume(**device, noise->GetShape(), Shape, 0);
    const std::vector<Texel> detail = ReadVolume(**device, noise->GetDetail(), Detail, 0);
    CHECK(CheckVolume(shape, Shape, 4, "forme"));
    CHECK(CheckVolume(detail, Detail, 3, "détail"));
    // Le Perlin-Worley fait des masses et du ciel entre elles, pas un brouillard uniforme ni un bloc plein.
    CHECK(Mean(shape, 0) > 0.2 && Mean(shape, 0) < 0.8);

    // Chaque mip moyenne le précédent : la moyenne ne bouge pas (à l'arrondi sur 8 bits près), et le dernier
    // niveau (1 pixel) vaut la moyenne de tout.
    const std::vector<Texel> half = ReadVolume(**device, noise->GetDetail(), Detail / 2, 1);
    const std::vector<Texel> last = ReadVolume(**device, noise->GetShape(), 1, 7);
    CHECK(half.size() == std::size_t{Detail / 2} * (Detail / 2) * (Detail / 2) && last.size() == 1);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        CHECK(std::abs(Mean(half, channel) - Mean(detail, channel)) < 0.01);
        CHECK(std::abs(last[0][channel] - Mean(shape, channel)) < 0.03);
    }
    return true;
}
