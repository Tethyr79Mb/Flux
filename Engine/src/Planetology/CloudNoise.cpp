#include "Planetology/CloudNoise.hpp"

#include "Flux/Platform/Filesystem.hpp"

#include <array>

namespace Flux::Planetology {
    namespace {
        // Doit correspondre à Constants dans shaders/CloudNoise.slang.
        struct Constants
        {
            std::uint32_t size;
        };
    }

    std::expected<CloudNoise, std::string> CloudNoise::Create(RHI::Device& device) {
        constexpr std::array bindings{RHI::Binding{0, RHI::BindingType::StorageTexture, RHI::ShaderStage::Compute}};
        auto layout = device.CreateBindGroupLayout({bindings, 2});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        const std::array<const RHI::BindGroupLayout*, 1> layouts{layout->get()};
        const std::filesystem::path shader = Platform::GetExecutableDirectory() / "shaders" / "CloudNoise.spv";
        auto shapePipeline = device.CreateComputePipeline({shader, "computeShape", layouts, sizeof(Constants)});
        auto detailPipeline = device.CreateComputePipeline({shader, "computeDetail", layouts, sizeof(Constants)});
        if (!shapePipeline || !detailPipeline) {
            return std::unexpected(!shapePipeline ? shapePipeline.error() : detailPipeline.error());
        }
        auto shape = device.CreateVolumeTexture(RHI::Format::RGBA8Unorm, {ShapeSize, ShapeSize}, ShapeSize, true);
        auto detail = device.CreateVolumeTexture(RHI::Format::RGBA8Unorm, {DetailSize, DetailSize}, DetailSize, true);
        if (!shape || !detail) {
            return std::unexpected(!shape ? shape.error() : detail.error());
        }
        const auto shapeGroup = device.CreateBindGroup(**layout);
        const auto detailGroup = device.CreateBindGroup(**layout);
        if (!shapeGroup || !detailGroup) {
            return std::unexpected("bruit des nuages : groupes de ressources");
        }
        shapeGroup->SetStorageTexture(0, **shape);
        detailGroup->SetStorageTexture(0, **detail);

        const bool computed = device.Submit([&](RHI::CommandList& commands) {
            const std::array uses{RHI::TextureUse{shape->get(), RHI::TextureAccess::Storage, true},
                                  RHI::TextureUse{detail->get(), RHI::TextureAccess::Storage, true}};
            commands.UseTextures(uses);
            const auto fill = [&](const RHI::Pipeline& pipeline, const RHI::BindGroup& group, std::uint32_t size) {
                const Constants constants{size};
                commands.BindPipeline(pipeline);
                commands.SetBindGroup(0, group);
                commands.PushConstants(&constants, sizeof(constants));
                commands.Dispatch(size / 4, size / 4, size / 4); // groupes de 4³ (numthreads)
            };
            fill(**shapePipeline, *shapeGroup, ShapeSize);
            fill(**detailPipeline, *detailGroup, DetailSize);
            commands.GenerateMipmaps(**shape);
            commands.GenerateMipmaps(**detail);
        });
        if (!computed) {
            return std::unexpected("bruit des nuages : calcul sur le GPU");
        }
        CloudNoise noise;
        noise.m_shape = std::move(*shape);
        noise.m_detail = std::move(*detail);
        return noise;
    }
}
