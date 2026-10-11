#include "Renderer/PlanetTileCompute.hpp"

#include "Flux/Platform/Filesystem.hpp"

namespace Flux {
    namespace {
        constexpr std::uint64_t VertexSize = Planet::TileVertexCount * sizeof(ModelVertex);
        constexpr std::uint64_t MorphSize = Planet::TileVertexCount * sizeof(Planet::MorphVertex);
        static_assert(sizeof(ModelVertex) == 8 * sizeof(float) && sizeof(Planet::MorphVertex) == 6 * sizeof(float),
                      "ModelVertex et MorphVertex doivent correspondre à Write dans shaders/PlanetTile.slang");

        // Doit correspondre à Constants dans shaders/PlanetTile.slang.
        struct Constants
        {
            std::uint32_t job;
            std::uint32_t firstVertex; // de la case dans sa page
        };
    }

    std::expected<std::unique_ptr<PlanetTileCompute>, std::string> PlanetTileCompute::Create(RHI::Device& device,
                                                                                           RHI::MemoryType memory) {
        std::unique_ptr<PlanetTileCompute> compute(new PlanetTileCompute(device, memory));
        constexpr std::array frameBindings{RHI::Binding{0, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Compute},
                                           RHI::Binding{1, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Compute}};
        auto frameLayout = device.CreateBindGroupLayout({frameBindings, RHI::FramesInFlight});
        auto pageLayout = device.CreateBindGroupLayout({frameBindings, MaxPages});
        if (!frameLayout || !pageLayout) {
            return std::unexpected(!frameLayout ? frameLayout.error() : pageLayout.error());
        }
        compute->m_frameLayout = std::move(*frameLayout);
        compute->m_pageLayout = std::move(*pageLayout);
        const std::array<const RHI::BindGroupLayout*, 2> layouts{compute->m_frameLayout.get(), compute->m_pageLayout.get()};
        auto pipeline = device.CreateComputePipeline(
            {Platform::GetExecutableDirectory() / "shaders" / "PlanetTile.spv", "computeMain", layouts, sizeof(Constants)});
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
        m_pages.clear();
    }

    std::expected<std::uint32_t, std::string> PlanetTileCompute::CreateTile() {
        if (m_tileCount == m_pages.size() * TilesPerPage) {
            if (m_pages.size() >= MaxPages) {
                return std::unexpected("trop de tuiles de planète");
            }
            const RHI::BufferUsage usage = RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage;
            auto vertices = m_device.CreateBuffer({TilesPerPage * VertexSize, usage, m_memory});
            auto morph = m_device.CreateBuffer({TilesPerPage * MorphSize, usage, m_memory});
            if (!vertices || !morph) {
                return std::unexpected(!vertices ? vertices.error() : morph.error());
            }
            Page page{std::move(*vertices), std::move(*morph), m_device.CreateBindGroup(*m_pageLayout)};
            if (!page.group) {
                return std::unexpected("trop de pages de tuiles de planète");
            }
            page.group->SetBuffer(0, *page.vertices);
            page.group->SetBuffer(1, *page.morph);
            m_pages.push_back(std::move(page));
        }
        return m_tileCount++;
    }

    void PlanetTileCompute::ReadTile(std::uint32_t tile, ModelVertex* vertices, Planet::MorphVertex* morph) const {
        const Page& page = m_pages[GetPage(tile)];
        const std::uint64_t slot = tile % TilesPerPage;
        page.vertices->Read(vertices, VertexSize, slot * VertexSize);
        page.morph->Read(morph, MorphSize, slot * MorphSize);
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
                                std::uint32_t tile) {
        Frame& frame = m_frames[frameIndex];
        if (frame.tiles.size() >= MaxTilesPerFrame) {
            return false;
        }
        Planet::PrepareTileGpu(terrain, key, m_inputs);
        frame.inputs->Write(&m_inputs, sizeof(m_inputs), frame.tiles.size() * sizeof(m_inputs));
        frame.tiles.push_back(tile);
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
        std::uint32_t boundPage = MaxPages;
        for (std::uint32_t job = 0; job < frame.computed; ++job) {
            const std::uint32_t tile = frame.tiles[job];
            if (GetPage(tile) != boundPage) {
                boundPage = GetPage(tile);
                commands.SetBindGroup(1, *m_pages[boundPage].group);
            }
            const Constants constants{job, GetFirstVertex(tile)};
            commands.PushConstants(&constants, sizeof(constants));
            commands.Dispatch(1);
        }
        commands.ComputeBarrier();
        frame.tiles.clear();
    }
}
