#include "Renderer/PlanetTileCompute.hpp"

#include "Flux/Platform/Filesystem.hpp"

namespace Flux {
    namespace {
        // Tuiles vivantes en même temps au plus (nœuds des planètes et buffers libres gardés).
        constexpr std::uint32_t MaxTileGroups = 4096;
        constexpr std::uint64_t VertexSize = Planet::TileVertexCount * sizeof(ModelVertex);
        constexpr std::uint64_t MorphSize = Planet::TileVertexCount * sizeof(Planet::MorphVertex);
        static_assert(sizeof(ModelVertex) == 8 * sizeof(float) && sizeof(Planet::MorphVertex) == 6 * sizeof(float),
                      "ModelVertex et MorphVertex doivent correspondre à Write dans shaders/PlanetTile.slang");
    }

    std::expected<std::unique_ptr<PlanetTileCompute>, std::string> PlanetTileCompute::Create(RHI::Device& device) {
        std::unique_ptr<PlanetTileCompute> compute(new PlanetTileCompute(device));
        constexpr std::array frameBindings{RHI::Binding{0, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Compute},
                                           RHI::Binding{1, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Compute}};
        auto frameLayout = device.CreateBindGroupLayout({frameBindings, RHI::FramesInFlight});
        auto tileLayout = device.CreateBindGroupLayout({frameBindings, MaxTileGroups});
        if (!frameLayout || !tileLayout) {
            return std::unexpected(!frameLayout ? frameLayout.error() : tileLayout.error());
        }
        compute->m_frameLayout = std::move(*frameLayout);
        compute->m_tileLayout = std::move(*tileLayout);
        const std::array<const RHI::BindGroupLayout*, 2> layouts{compute->m_frameLayout.get(), compute->m_tileLayout.get()};
        auto pipeline = device.CreateComputePipeline(
            {Platform::GetExecutableDirectory() / "shaders" / "PlanetTile.spv", "computeMain", layouts, sizeof(std::uint32_t)});
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        compute->m_pipeline = std::move(*pipeline);

        for (Frame& frame : compute->m_frames) {
            auto inputs = device.CreateBuffer({MaxTilesPerFrame * sizeof(Planet::TileGpuInputs), RHI::BufferUsage::Storage, RHI::MemoryType::Upload});
            auto stats = device.CreateBuffer({MaxTilesPerFrame * sizeof(Stats), RHI::BufferUsage::Storage, RHI::MemoryType::Readback});
            if (!inputs || !stats) {
                return std::unexpected(!inputs ? inputs.error() : stats.error());
            }
            frame.inputs = std::move(*inputs);
            frame.stats = std::move(*stats);
            frame.group = device.CreateBindGroup(*compute->m_frameLayout);
            frame.group->SetBuffer(0, *frame.inputs);
            frame.group->SetBuffer(1, *frame.stats);
            frame.tiles.reserve(MaxTilesPerFrame);
        }
        return compute;
    }

    PlanetTileCompute::~PlanetTileCompute() {
        for (Frame& frame : m_frames) {
            frame.group.reset(); // avant leur layout
        }
    }

    std::expected<PlanetTileCompute::TileBuffers, std::string> PlanetTileCompute::CreateTileBuffers(RHI::MemoryType memory) {
        const RHI::BufferUsage usage = RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage;
        auto vertices = m_device.CreateBuffer({VertexSize, usage, memory});
        auto morph = m_device.CreateBuffer({MorphSize, usage, memory});
        if (!vertices || !morph) {
            return std::unexpected(!vertices ? vertices.error() : morph.error());
        }
        TileBuffers buffers{std::move(*vertices), std::move(*morph), m_device.CreateBindGroup(*m_tileLayout)};
        if (!buffers.group) {
            return std::unexpected("trop de tuiles de planète");
        }
        buffers.group->SetBuffer(0, *buffers.vertices);
        buffers.group->SetBuffer(1, *buffers.morph);
        return buffers;
    }

    std::span<const PlanetTileCompute::Stats> PlanetTileCompute::GetStats(std::uint32_t frameIndex) {
        Frame& frame = m_frames[frameIndex];
        frame.readStats.resize(frame.computed);
        if (frame.computed > 0) {
            frame.stats->Read(frame.readStats.data(), frame.computed * sizeof(Stats));
        }
        return frame.readStats;
    }

    bool PlanetTileCompute::Add(std::uint32_t frameIndex, const Planet::Terrain& terrain, Planet::TileKey key,
                                const TileBuffers& buffers) {
        Frame& frame = m_frames[frameIndex];
        if (frame.tiles.size() >= MaxTilesPerFrame) {
            return false;
        }
        Planet::PrepareTileGpu(terrain, key, m_inputs);
        frame.inputs->Write(&m_inputs, sizeof(m_inputs), frame.tiles.size() * sizeof(m_inputs));
        frame.tiles.push_back(buffers.group.get());
        return true;
    }

    void PlanetTileCompute::Record(RHI::CommandList& commands, std::uint32_t frameIndex) {
        Frame& frame = m_frames[frameIndex];
        frame.computed = static_cast<std::uint32_t>(frame.tiles.size());
        if (frame.tiles.empty()) {
            return;
        }
        commands.BindPipeline(*m_pipeline);
        commands.SetBindGroup(0, *frame.group);
        for (std::uint32_t job = 0; job < frame.computed; ++job) {
            commands.SetBindGroup(1, *frame.tiles[job]);
            commands.PushConstants(&job, sizeof(job));
            commands.Dispatch(1);
        }
        commands.ComputeBarrier();
        frame.tiles.clear();
    }
}
