// Tests des envois vers la mémoire vidéo (Renderer/Vulkan/VulkanTransfer) : buffers et textures envoyés sans fenêtre,
// puis relus par le CPU. Ignorés (réussis) sur une machine sans GPU Vulkan 1.3, sauf avec FLUX_TESTS_GPU=1 (la CI,
// sur lavapipe, qui n'a qu'une file : le chemin sans file de transfert séparée).
#include "Check.hpp"

#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace Flux;

namespace {
    // Valeur du mot `index` du motif `seed` : chaque envoi a le sien, un mélange se voit.
    std::uint32_t Pattern(std::uint32_t seed, std::uint64_t index) {
        return seed * 2654435761u ^ static_cast<std::uint32_t>(index * 40503u);
    }

    std::vector<std::uint32_t> MakePattern(std::uint32_t seed, std::uint64_t words) {
        std::vector<std::uint32_t> data(words);
        for (std::uint64_t i = 0; i < words; ++i) {
            data[i] = Pattern(seed, i);
        }
        return data;
    }

    // Copie `size` octets de `buffer` dans un buffer Readback et les relit.
    std::vector<std::uint32_t> ReadBack(RHI::Device& device, const RHI::Buffer& buffer, std::uint64_t offset, std::uint64_t size) {
        auto readback = device.CreateBuffer({size, RHI::BufferUsage::Storage, RHI::MemoryType::Readback});
        if (!readback) {
            return {};
        }
        const bool submitted = device.Submit([&](RHI::CommandList& commands) {
            commands.CopyBuffer(buffer, offset, **readback, 0, size);
        });
        if (!submitted) {
            return {};
        }
        std::vector<std::uint32_t> data(size / sizeof(std::uint32_t));
        (*readback)->Read(data.data(), size);
        return data;
    }

    bool SameAsPattern(const std::vector<std::uint32_t>& data, std::uint32_t seed, std::uint64_t first) {
        for (std::uint64_t i = 0; i < data.size(); ++i) {
            if (data[i] != Pattern(seed, first + i)) {
                std::printf("       mot %llu : %08x au lieu de %08x\n", static_cast<unsigned long long>(first + i), data[i],
                            Pattern(seed, first + i));
                return false;
            }
        }
        return true;
    }

    bool TestBuffers(RHI::Device& device) {
        // Buffer neuf rempli à la création, puis réécrit en partie deux fois au même endroit : le dernier gagne.
        constexpr std::uint64_t Words = 1 << 18; // 1 Mo
        constexpr std::uint64_t Size = Words * sizeof(std::uint32_t);
        const std::vector<std::uint32_t> initial = MakePattern(1, Words);
        auto buffer = device.CreateBuffer({Size, RHI::BufferUsage::Storage, RHI::MemoryType::GpuOnly}, initial.data());
        CHECK(buffer.has_value());
        const std::vector<std::uint32_t> first = MakePattern(2, 1000);
        const std::vector<std::uint32_t> second = MakePattern(3, 1000);
        CHECK(device.WriteBuffer(**buffer, 4000, first.data(), 4000));
        CHECK(device.WriteBuffer(**buffer, 4000, second.data(), 4000));
        const std::vector<std::uint32_t> head = ReadBack(device, **buffer, 0, 8000);
        CHECK(head.size() == 2000);
        CHECK(SameAsPattern({head.begin(), head.begin() + 1000}, 1, 0));
        CHECK(SameAsPattern({head.begin() + 1000, head.end()}, 3, 0));
        CHECK(SameAsPattern(ReadBack(device, **buffer, Size - 4096, 4096), 1, Words - 1024));

        // Plus que l'anneau d'envoi (64 Mo) en morceaux : il fait le tour sans écraser ce qui n'est pas encore copié.
        constexpr std::uint64_t Chunks = 80;
        auto large = device.CreateBuffer({Chunks * Size, RHI::BufferUsage::Storage, RHI::MemoryType::GpuOnly});
        CHECK(large.has_value());
        for (std::uint32_t chunk = 0; chunk < Chunks; ++chunk) {
            const std::vector<std::uint32_t> data = MakePattern(100 + chunk, Words);
            CHECK(device.WriteBuffer(**large, chunk * Size, data.data(), Size));
        }
        for (std::uint32_t chunk = 0; chunk < Chunks; chunk += 13) {
            CHECK(SameAsPattern(ReadBack(device, **large, chunk * Size + Size - 1024, 1024), 100 + chunk, Words - 256));
        }

        // Plus que la moitié de l'anneau d'un coup : son propre buffer d'envoi.
        constexpr std::uint64_t BigWords = 10 << 20; // 40 Mo
        const std::vector<std::uint32_t> big = MakePattern(7, BigWords);
        auto bigBuffer = device.CreateBuffer({BigWords * sizeof(std::uint32_t), RHI::BufferUsage::Storage, RHI::MemoryType::GpuOnly},
                                             big.data());
        CHECK(bigBuffer.has_value());
        CHECK(SameAsPattern(ReadBack(device, **bigBuffer, (BigWords - 1024) * sizeof(std::uint32_t), 4096), 7, BigWords - 1024));
        return true;
    }

    // Texture 4 x 4 avec mipmaps (niveaux réduits par la file graphique après la copie) : relue par un compute shader.
    bool TestTexture(RHI::Device& device) {
        constexpr std::uint32_t Width = 4;
        std::vector<std::uint8_t> pixels(Width * Width * 4);
        std::array<double, 4> sum{};
        for (std::uint32_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<std::uint8_t>((i * 37 + 11) % 256);
            sum[i % 4] += pixels[i];
        }
        auto texture = device.CreateTexture(RHI::Format::RGBA8Unorm, {Width, Width}, pixels, true);
        auto sampler = device.CreateSampler({RHI::Filter::Nearest, RHI::Filter::Nearest, RHI::AddressMode::ClampToEdge});
        auto texels = device.CreateBuffer({Width * Width * 16, RHI::BufferUsage::Storage, RHI::MemoryType::Readback});
        CHECK(texture && sampler && texels);

        constexpr std::array bindings{RHI::Binding{0, RHI::BindingType::Texture, RHI::ShaderStage::Compute},
                                      RHI::Binding{1, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Compute}};
        auto layout = device.CreateBindGroupLayout({bindings, 1});
        CHECK(layout.has_value());
        const std::array<const RHI::BindGroupLayout*, 1> layouts{layout->get()};
        struct Constants
        {
            std::uint32_t width;
            std::uint32_t level;
        };
        auto pipeline = device.CreateComputePipeline({"shaders/TransferTests.spv", "computeReadTexture", layouts, sizeof(Constants)});
        if (!pipeline) {
            std::printf("ÉCHEC %s\n", pipeline.error().c_str());
            return false;
        }
        auto group = device.CreateBindGroup(**layout);
        CHECK(group != nullptr);
        group->SetTexture(0, **texture, **sampler);
        group->SetBuffer(1, **texels);

        std::vector<float> values(Width * Width * 4);
        for (const std::uint32_t level : {0u, 2u}) {
            const Constants constants{Width >> level, level};
            CHECK(device.Submit([&](RHI::CommandList& commands) {
                commands.BindPipeline(**pipeline);
                commands.SetBindGroup(0, *group);
                commands.PushConstants(&constants, sizeof(constants));
                commands.Dispatch(1, 1);
                commands.ComputeBarrier();
            }));
            (*texels)->Read(values.data(), values.size() * sizeof(float));
            if (level == 0) {
                for (std::uint32_t i = 0; i < pixels.size(); ++i) {
                    CHECK(std::abs(values[i] * 255.0f - pixels[i]) < 0.01f);
                }
            } else {
                // 1 x 1 : la moyenne des 16 pixels (deux réductions de moitié, arrondies sur 8 bits entre les deux).
                for (std::uint32_t channel = 0; channel < 4; ++channel) {
                    CHECK(std::abs(values[channel] * 255.0f - sum[channel] / 16.0) < 2.0f);
                }
            }
        }
        return true;
    }
}

bool RunTransferTests() {
    auto device = RHI::Device::CreateHeadless();
    if (!device) {
        return GpuMissing("envois", device.error().c_str());
    }
    return TestBuffers(**device) && TestTexture(**device);
}
