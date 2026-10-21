// Tests des compute shaders du RHI : un calcul envoyé au GPU sans fenêtre, puis relu par le CPU. Ignorés (réussis)
// sur une machine sans GPU Vulkan 1.3, sauf avec FLUX_TESTS_GPU=1 (la CI, sur lavapipe).
#include "Check.hpp"

#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <cmath>
#include <vector>

using namespace Flux;

bool RunComputeTests() {
    auto device = RHI::Device::CreateHeadless();
    if (!device) {
        return GpuMissing("compute", device.error().c_str());
    }

    constexpr std::uint32_t Count = 1000; // pas un multiple de 64 : le dernier groupe déborde
    std::vector<float> input(Count);
    for (std::uint32_t i = 0; i < Count; ++i) {
        input[i] = static_cast<float>(i) * 0.5f - 100.0f;
    }
    constexpr std::uint64_t Size = Count * sizeof(float);
    auto inputBuffer = (*device)->CreateBuffer({Size, RHI::BufferUsage::Storage, RHI::MemoryType::GpuOnly}, input.data());
    auto outputBuffer = (*device)->CreateBuffer({Size, RHI::BufferUsage::Storage | RHI::BufferUsage::Vertex, RHI::MemoryType::Readback});
    CHECK(inputBuffer && outputBuffer);

    constexpr std::array bindings{RHI::Binding{0, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Compute},
                                  RHI::Binding{1, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Compute}};
    auto layout = (*device)->CreateBindGroupLayout({bindings, 1});
    CHECK(layout.has_value());
    const std::array<const RHI::BindGroupLayout*, 1> layouts{layout->get()};
    struct Constants
    {
        std::uint32_t count;
        float scale;
    };
    auto pipeline = (*device)->CreateComputePipeline({"shaders/ComputeTests.spv", "computeMain", layouts, sizeof(Constants)});
    auto doubling = (*device)->CreateComputePipeline({"shaders/ComputeTests.spv", "computeDouble", layouts, sizeof(Constants)});
    if (!pipeline || !doubling) {
        std::printf("ÉCHEC %s\n", (pipeline ? doubling.error() : pipeline.error()).c_str());
        return false;
    }
    {
        auto group = (*device)->CreateBindGroup(**layout);
        CHECK(group != nullptr);
        group->SetBuffer(0, **inputBuffer);
        group->SetBuffer(1, **outputBuffer);

        const Constants constants{Count, 3.0f};
        const bool submitted = (*device)->Submit([&](RHI::CommandList& commands) {
            commands.BindPipeline(**pipeline);
            commands.SetBindGroup(0, *group);
            commands.PushConstants(&constants, sizeof(constants));
            commands.Dispatch((Count + 63) / 64);
            commands.ComputeBarrier();
            commands.BindPipeline(**doubling);
            commands.SetBindGroup(0, *group);
            commands.PushConstants(&constants, sizeof(constants));
            commands.Dispatch((Count + 63) / 64);
            commands.ComputeBarrier();
        });
        CHECK(submitted);
    }

    std::vector<float> output(Count);
    (*outputBuffer)->Read(output.data(), Size);
    for (std::uint32_t i = 0; i < Count; ++i) {
        const float expected = (input[i] * 3.0f + static_cast<float>(i)) * 2.0f;
        CHECK(std::abs(output[i] - expected) < 1.0e-3f);
    }
    return true;
}
