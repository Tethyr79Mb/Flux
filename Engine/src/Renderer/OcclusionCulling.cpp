#include "Renderer/OcclusionCulling.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <bit>

namespace Flux {
    namespace {
        // Doit correspondre à CullFrame dans shaders/Culling.slang.
        struct CullFrame
        {
            Math::Matrix4f previousViewProjection;
            std::array<std::uint32_t, 4> size; // profondeur source, niveaux, entités
            std::array<std::uint32_t, 16> levelOffsets;
        };
        static_assert(sizeof(CullFrame) == 144);
        static_assert(sizeof(OcclusionCulling::Item) == 32);

        // Doit correspondre à PyramidConstants dans shaders/Culling.slang.
        struct PyramidConstants
        {
            std::uint32_t sourceOffset;
            std::uint32_t sourceWidth;
            std::uint32_t sourceHeight;
            std::uint32_t targetOffset;
            std::uint32_t targetWidth;
            std::uint32_t targetHeight;
            std::uint32_t fromDepth;
            std::uint32_t padding;
        };

        constexpr std::uint32_t GroupSize = 64; // GroupSize dans shaders/Culling.slang
        constexpr std::uint32_t PyramidGroupSize = 8;
        constexpr std::uint64_t MinItemBufferSize = 64 * 1024;
        constexpr std::uint32_t MaxLevels = 16;

        // Emplacements du groupe (voir shaders/Culling.slang).
        constexpr std::uint32_t FrameSlot = 0;
        constexpr std::uint32_t ItemSlot = 1;
        constexpr std::uint32_t DrawSlot = 2;
        constexpr std::uint32_t CommandSlot = 3;
        constexpr std::uint32_t InstanceSlot = 4;
        constexpr std::uint32_t SourcePyramidSlot = 5;
        constexpr std::uint32_t StatsSlot = 6;
        constexpr std::uint32_t DepthSlot = 7;
        constexpr std::uint32_t TargetPyramidSlot = 8;
    }

    std::expected<std::unique_ptr<OcclusionCulling>, std::string> OcclusionCulling::Create(RHI::Device& device) {
        std::unique_ptr<OcclusionCulling> culling(new OcclusionCulling(device));
        constexpr auto compute = RHI::ShaderStage::Compute;
        constexpr std::array bindings{
            RHI::Binding{FrameSlot, RHI::BindingType::StorageBuffer, compute},
            RHI::Binding{ItemSlot, RHI::BindingType::StorageBuffer, compute},
            RHI::Binding{DrawSlot, RHI::BindingType::StorageBuffer, compute},
            RHI::Binding{CommandSlot, RHI::BindingType::StorageBuffer, compute},
            RHI::Binding{InstanceSlot, RHI::BindingType::StorageBuffer, compute},
            RHI::Binding{SourcePyramidSlot, RHI::BindingType::StorageBuffer, compute},
            RHI::Binding{StatsSlot, RHI::BindingType::StorageBuffer, compute},
            RHI::Binding{DepthSlot, RHI::BindingType::Texture, compute},
            RHI::Binding{TargetPyramidSlot, RHI::BindingType::StorageBuffer, compute},
        };
        auto layout = device.CreateBindGroupLayout({bindings, 2 * RHI::FramesInFlight});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        culling->m_layout = std::move(*layout);
        const std::array<const RHI::BindGroupLayout*, 1> layouts{culling->m_layout.get()};
        const auto shader = Platform::GetExecutableDirectory() / "shaders" / "Culling.spv";
        auto clear = device.CreateComputePipeline({shader, "clearMain", layouts, 0});
        auto cull = device.CreateComputePipeline({shader, "cullMain", layouts, 0});
        auto pyramid = device.CreateComputePipeline({shader, "pyramidMain", layouts, sizeof(PyramidConstants)});
        if (!clear || !cull || !pyramid) {
            return std::unexpected(!clear ? clear.error() : !cull ? cull.error() : pyramid.error());
        }
        culling->m_clear = std::move(*clear);
        culling->m_cull = std::move(*cull);
        culling->m_pyramidPipeline = std::move(*pyramid);
        auto sampler = device.CreateSampler({.filter = RHI::Filter::Nearest, .mipFilter = RHI::Filter::Nearest,
                                             .address = RHI::AddressMode::ClampToEdge});
        if (!sampler) {
            return std::unexpected(sampler.error());
        }
        culling->m_sampler = std::move(*sampler);

        for (Frame& frame : culling->m_frames) {
            auto data = device.CreateBuffer({sizeof(CullFrame), RHI::BufferUsage::Storage, RHI::MemoryType::Upload});
            auto items = device.CreateBuffer({MinItemBufferSize, RHI::BufferUsage::Storage, RHI::MemoryType::Upload});
            auto stats = device.CreateBuffer({16, RHI::BufferUsage::Storage, RHI::MemoryType::Readback});
            if (!data || !items || !stats) {
                return std::unexpected(!data ? data.error() : !items ? items.error() : stats.error());
            }
            frame.frame = std::move(*data);
            frame.items = std::move(*items);
            frame.stats = std::move(*stats);
            frame.group = device.CreateBindGroup(*culling->m_layout);
            frame.pyramidGroup = device.CreateBindGroup(*culling->m_layout);
            if (!frame.group || !frame.pyramidGroup) {
                return std::unexpected("allocation des groupes de l'élimination des objets cachés");
            }
        }
        return culling;
    }

    OcclusionCulling::~OcclusionCulling() {
        for (Frame& frame : m_frames) {
            frame.group.reset(); // avant leur layout
            frame.pyramidGroup.reset();
        }
    }

    bool OcclusionCulling::Begin(std::uint32_t frameIndex, bool continuous, const Math::Matrix4f& previousViewProjection) {
        ++m_frame;
        Frame& frame = m_frames[frameIndex];
        m_pyramids[frameIndex].retired.reset(); // l'image qui la lisait est finie
        m_stats = {};
        if (frame.testing) {
            m_stats.tested = frame.tested;
            frame.stats->Read(&m_stats.hidden, sizeof(m_stats.hidden));
        }
        frame.testing = false;
        frame.tested = 0;
        m_items.clear();
        m_source = m_lastPyramid;
        m_previousViewProjection = previousViewProjection;
        const Pyramid& previous = m_pyramids[m_source];
        return m_enabled && continuous && m_source != frameIndex && previous.frame != 0 && previous.frame + 1 == m_frame;
    }

    bool OcclusionCulling::Upload(std::uint32_t frameIndex, const RHI::Buffer& draws, const RHI::Buffer& commands,
                                  const RHI::Buffer& instances) {
        Frame& frame = m_frames[frameIndex];
        if (m_items.empty()) {
            return true;
        }
        const std::uint64_t bytes = m_items.size() * sizeof(Item);
        if (frame.items->GetSize() < bytes) {
            auto grown = m_device.CreateBuffer({std::bit_ceil(bytes), RHI::BufferUsage::Storage, RHI::MemoryType::Upload});
            if (!grown) {
                Log::Error("{}", grown.error());
                return false;
            }
            frame.items = std::move(*grown);
        }
        const Pyramid& source = m_pyramids[m_source];
        CullFrame data{m_previousViewProjection,
                       {source.size.width, source.size.height, source.levels, static_cast<std::uint32_t>(m_items.size())},
                       source.offsets};
        frame.frame->Write(&data, sizeof(data));
        frame.items->Write(m_items.data(), bytes);
        frame.group->SetBuffer(FrameSlot, *frame.frame);
        frame.group->SetBuffer(ItemSlot, *frame.items);
        frame.group->SetBuffer(DrawSlot, draws);
        frame.group->SetBuffer(CommandSlot, commands);
        frame.group->SetBuffer(InstanceSlot, instances);
        frame.group->SetBuffer(SourcePyramidSlot, *source.buffer);
        frame.group->SetBuffer(StatsSlot, *frame.stats);
        frame.testing = true;
        frame.tested = static_cast<std::uint32_t>(m_items.size());
        return true;
    }

    void OcclusionCulling::Record(RHI::CommandList& commands, std::uint32_t frameIndex) {
        const Frame& frame = m_frames[frameIndex];
        if (!frame.testing) {
            return;
        }
        commands.BindPipeline(*m_clear);
        commands.SetBindGroup(0, *frame.group);
        commands.Dispatch(1);
        commands.ComputeBarrier();
        commands.BindPipeline(*m_cull);
        commands.SetBindGroup(0, *frame.group);
        commands.Dispatch((frame.tested + GroupSize - 1) / GroupSize);
        commands.ComputeBarrier(); // commandes et entités lues par les dessins
    }

    void OcclusionCulling::BuildPyramid(RHI::CommandList& commands, std::uint32_t frameIndex, const RHI::Texture& depth,
                                        RHI::Extent size) {
        if (!m_enabled || size.width == 0 || size.height == 0) {
            return;
        }
        Pyramid& pyramid = m_pyramids[frameIndex];
        // Niveaux jusqu'à un seul texel : la source divisée par deux à chaque fois, arrondie au-dessus.
        std::array<RHI::Extent, MaxLevels> sizes{};
        std::uint32_t levels = 0;
        std::uint64_t texels = 0;
        RHI::Extent level = size;
        while ((level.width > 1 || level.height > 1 || levels == 0) && levels < MaxLevels) {
            level = {(level.width + 1) / 2, (level.height + 1) / 2};
            sizes[levels] = level;
            pyramid.offsets[levels] = static_cast<std::uint32_t>(texels);
            texels += static_cast<std::uint64_t>(level.width) * level.height;
            ++levels;
        }
        const std::uint64_t bytes = texels * sizeof(float);
        if (!pyramid.buffer || pyramid.buffer->GetSize() < bytes) {
            auto grown = m_device.CreateBuffer({std::bit_ceil(bytes), RHI::BufferUsage::Storage, RHI::MemoryType::GpuOnly});
            if (!grown) {
                Log::Error("{}", grown.error());
                pyramid.frame = 0;
                return;
            }
            pyramid.retired = std::move(pyramid.buffer); // l'image en vol la lit peut-être encore
            pyramid.buffer = std::move(*grown);
        }
        Frame& frame = m_frames[frameIndex];
        frame.pyramidGroup->SetTexture(DepthSlot, depth, *m_sampler);
        frame.pyramidGroup->SetBuffer(TargetPyramidSlot, *pyramid.buffer);

        commands.ComputeBarrier(); // l'image en vol a fini de lire ce buffer (son test) avant qu'il soit réécrit
        commands.BindPipeline(*m_pyramidPipeline);
        commands.SetBindGroup(0, *frame.pyramidGroup);
        RHI::Extent source = size;
        for (std::uint32_t l = 0; l < levels; ++l) {
            const PyramidConstants constants{l == 0 ? 0u : pyramid.offsets[l - 1], source.width, source.height, pyramid.offsets[l],
                                             sizes[l].width, sizes[l].height, l == 0 ? 1u : 0u, 0u};
            commands.PushConstants(&constants, sizeof(constants));
            commands.Dispatch((sizes[l].width + PyramidGroupSize - 1) / PyramidGroupSize,
                              (sizes[l].height + PyramidGroupSize - 1) / PyramidGroupSize);
            commands.ComputeBarrier();
            source = sizes[l];
        }
        pyramid.size = size;
        pyramid.levels = levels;
        pyramid.frame = m_frame;
        m_lastPyramid = frameIndex;
    }
}
