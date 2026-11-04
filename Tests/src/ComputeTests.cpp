// Tests des compute shaders du RHI : un calcul envoyé au GPU sans fenêtre, puis relu par le CPU ; le même calcul
// enregistré dans des listes parallèles (CommandList::Fork) remplies par des fils de travail ; une passe de dessin
// remplie par des listes secondaires. Ignorés (réussis) sur une
// machine sans GPU Vulkan 1.3, sauf avec FLUX_TESTS_GPU=1 (la CI, sur lavapipe).
#include "Check.hpp"

#include "Flux/Core/JobSystem.hpp"
#include "Flux/Renderer/RHI/RHI.hpp"

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

    // Listes parallèles : remplies dans le désordre par des fils, jouées dans l'ordre du tableau, puis la liste
    // principale reprend (pipeline à relier). Première liste : le calcul ; les trois suivantes et la suite : doublent.
    // Fork refusé sur une liste parallèle et au-delà de MaxParallelCommandLists.
    {
        auto group = (*device)->CreateBindGroup(**layout);
        CHECK(group != nullptr);
        group->SetBuffer(0, **inputBuffer);
        group->SetBuffer(1, **outputBuffer);
        const Constants constants{Count, 3.0f};
        JobSystem jobs(3);
        bool forked = false;
        bool refused = false;
        bool tooMany = false;
        const bool submitted = (*device)->Submit([&](RHI::CommandList& commands) {
            const std::span<RHI::CommandList* const> lists = commands.Fork(4);
            forked = lists.size() == 4;
            if (!forked) {
                return;
            }
            refused = lists[0]->Fork(2).empty(); // pas de Fork sur une liste parallèle
            jobs.ParallelFor(4, 1, [&](std::uint32_t begin, std::uint32_t end) {
                for (std::uint32_t i = begin; i < end; ++i) {
                    RHI::CommandList& list = *lists[i];
                    list.BeginStep("calcul parallèle");
                    list.BindPipeline(i == 0 ? **pipeline : **doubling);
                    list.SetBindGroup(0, *group);
                    list.PushConstants(&constants, sizeof(constants));
                    list.Dispatch((Count + 63) / 64);
                    list.ComputeBarrier();
                }
            });
            tooMany = commands.Fork(RHI::MaxParallelCommandLists).empty(); // 4 déjà pris
            commands.BindPipeline(**doubling);
            commands.SetBindGroup(0, *group);
            commands.PushConstants(&constants, sizeof(constants));
            commands.Dispatch((Count + 63) / 64);
            commands.ComputeBarrier();
        });
        CHECK(submitted && forked && refused && tooMany);
    }
    (*outputBuffer)->Read(output.data(), Size);
    for (std::uint32_t i = 0; i < Count; ++i) {
        const float expected = (input[i] * 3.0f + static_cast<float>(i)) * 16.0f;
        CHECK(std::abs(output[i] - expected) < 1.0e-2f);
    }

    // Passe de dessin remplie par des listes secondaires (BeginParallelRenderPass), chacune par un fil, dans une liste
    // parallèle : la soumission passe, les étapes de plantage et les heures des listes secondaires comprises.
    {
        auto color = (*device)->CreateRenderTarget(RHI::Format::RGBA16Float, {64, 64}, true);
        auto depth = (*device)->CreateRenderTarget(RHI::Format::D32Float, {64, 64}, true);
        CHECK(color.has_value() && depth.has_value());
        JobSystem jobs(3);
        bool opened = false;
        const bool submitted = (*device)->Submit([&](RHI::CommandList& commands) {
            const std::span<RHI::CommandList* const> lists = commands.Fork(1);
            RHI::CommandList& list = lists.empty() ? commands : *lists[0];
            list.Use(**color, RHI::TextureAccess::ColorTarget, true);
            list.Use(**depth, RHI::TextureAccess::DepthTarget, true);
            const std::span<RHI::CommandList* const> inside = list.BeginParallelRenderPass(
                {.clearColor = {0.5f, 0.5f, 0.5f, 1.0f}, .depth = depth->get(), .colorTarget = color->get()}, 4);
            opened = inside.size() == 4;
            if (opened) {
                jobs.ParallelFor(4, 1, [&](std::uint32_t begin, std::uint32_t end) {
                    for (std::uint32_t i = begin; i < end; ++i) {
                        // Un quart de l'image chacune.
                        const std::int32_t x = static_cast<std::int32_t>(i % 2) * 32;
                        const std::int32_t y = static_cast<std::int32_t>(i / 2) * 32;
                        inside[i]->BeginStep("quart");
                        inside[i]->WriteTimestamp(i);
                        inside[i]->SetViewport({static_cast<float>(x), static_cast<float>(y), 32.0f, 32.0f});
                        inside[i]->SetScissor({x, y, 32, 32});
                        inside[i]->ClearDepth({x, y, 32, 32}, 0.25f * static_cast<float>(i));
                    }
                });
            }
            list.EndRenderPass();
        });
        CHECK(submitted && opened);
    }
    return true;
}
