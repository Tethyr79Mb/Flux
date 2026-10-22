#include "Planetology/VegetationRenderer.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <utility>

namespace Flux::Planetology {
    namespace {
        // Doit correspondre à Constants dans shaders/PlanetTile.slang (le semis y lit job seulement).
        struct SowConstants
        {
            std::uint32_t job;
            std::uint32_t unused = 0;
        };
        static_assert(sizeof(TileGpuInputs) % 16 == 0);

        // Bord des tuiles qui servent de plaines, au plus (m).
        constexpr double MaxPlainEdge = 400.0;
        constexpr std::uint32_t CounterOffset = VegetationRenderer::MaxCommands * 5; // en entiers, dans les commandes

        // Emplacements du groupe du semis (shaders/PlanetTile.slang) et du tri (shaders/Vegetation.slang).
        constexpr std::uint32_t SowInputsSlot = 0;
        constexpr std::uint32_t SowJobsSlot = 2;
        constexpr std::uint32_t SowTreesSlot = 3;
        constexpr std::uint32_t SowCountsSlot = 4;
        constexpr std::uint32_t CullFrameSlot = 0;
        constexpr std::uint32_t CullCellsSlot = 1;
        constexpr std::uint32_t CullTreesSlot = 2;
        constexpr std::uint32_t CullCountsSlot = 3;
        constexpr std::uint32_t CullInstancesSlot = 4;
        constexpr std::uint32_t CullCommandsSlot = 5;
        constexpr std::uint32_t CullStatsSlot = 6;

        constexpr std::uint32_t InstanceCount() {
            std::uint32_t total = 0;
            for (const std::uint32_t capacity : VegetationRenderer::Capacities) {
                total += capacity;
            }
            return total;
        }
    }

    std::uint32_t VegetationRenderer::GetPlainLevel(const Terrain& terrain) {
        // Bord d'une tuile de niveau n : environ (π/2) R / 2^n.
        const double edge = 0.5 * std::numbers::pi * terrain.GetRadius();
        const double level = std::ceil(std::log2(std::max(edge / MaxPlainEdge, 1.0)));
        return std::min(static_cast<std::uint32_t>(level), terrain.GetMaxLevel());
    }

    std::expected<std::unique_ptr<VegetationRenderer>, std::string> VegetationRenderer::Create(RHI::Device& device,
                                                                                               RHI::MemoryType memory) {
        std::unique_ptr<VegetationRenderer> renderer(new VegetationRenderer(device));
        constexpr auto compute = RHI::ShaderStage::Compute;
        constexpr std::array sowBindings{RHI::Binding{SowInputsSlot, RHI::BindingType::StorageBuffer, compute},
                                         RHI::Binding{SowJobsSlot, RHI::BindingType::StorageBuffer, compute},
                                         RHI::Binding{SowTreesSlot, RHI::BindingType::StorageBuffer, compute},
                                         RHI::Binding{SowCountsSlot, RHI::BindingType::StorageBuffer, compute}};
        constexpr std::array cullBindings{RHI::Binding{CullFrameSlot, RHI::BindingType::StorageBuffer, compute},
                                          RHI::Binding{CullCellsSlot, RHI::BindingType::StorageBuffer, compute},
                                          RHI::Binding{CullTreesSlot, RHI::BindingType::StorageBuffer, compute},
                                          RHI::Binding{CullCountsSlot, RHI::BindingType::StorageBuffer, compute},
                                          RHI::Binding{CullInstancesSlot, RHI::BindingType::StorageBuffer, compute},
                                          RHI::Binding{CullCommandsSlot, RHI::BindingType::StorageBuffer, compute},
                                          RHI::Binding{CullStatsSlot, RHI::BindingType::StorageBuffer, compute}};
        auto sowLayout = device.CreateBindGroupLayout({sowBindings, RHI::FramesInFlight});
        auto cullLayout = device.CreateBindGroupLayout({cullBindings, RHI::FramesInFlight});
        if (!sowLayout || !cullLayout) {
            return std::unexpected(!sowLayout ? sowLayout.error() : cullLayout.error());
        }
        renderer->m_sowLayout = std::move(*sowLayout);
        renderer->m_cullLayout = std::move(*cullLayout);
        const std::filesystem::path shaders = Platform::GetExecutableDirectory() / "shaders";
        const std::array<const RHI::BindGroupLayout*, 1> sowLayouts{renderer->m_sowLayout.get()};
        const std::array<const RHI::BindGroupLayout*, 1> cullLayouts{renderer->m_cullLayout.get()};
        auto sow = device.CreateComputePipeline({shaders / (device.HasFloat64() ? "PlanetTile64.spv" : "PlanetTile.spv"), "computeTrees", sowLayouts, sizeof(SowConstants)});
        auto cull = device.CreateComputePipeline({shaders / "Vegetation.spv", "computeCull", cullLayouts, 0});
        auto finalize = device.CreateComputePipeline({shaders / "Vegetation.spv", "computeFinalize", cullLayouts, 0});
        if (!sow || !cull || !finalize) {
            return std::unexpected(!sow ? sow.error() : !cull ? cull.error() : finalize.error());
        }
        renderer->m_sow = std::move(*sow);
        renderer->m_cull = std::move(*cull);
        renderer->m_finalize = std::move(*finalize);

        static_assert(sizeof(Tree) == 32);
        auto trees = device.CreateBuffer({std::uint64_t{MaxPlains} * TreesPerPlain * sizeof(Tree), RHI::BufferUsage::Storage, memory});
        auto counts = device.CreateBuffer({MaxPlains * sizeof(std::uint32_t), RHI::BufferUsage::Storage, memory});
        if (!trees || !counts) {
            return std::unexpected(!trees ? trees.error() : counts.error());
        }
        renderer->m_trees = std::move(*trees);
        renderer->m_treeCounts = std::move(*counts);

        for (Frame& frame : renderer->m_frames) {
            auto inputs = device.CreateBuffer({MaxSownPerFrame * sizeof(TileGpuInputs), RHI::BufferUsage::Storage, RHI::MemoryType::Upload});
            auto jobs = device.CreateBuffer({MaxSownPerFrame * sizeof(TreeJob), RHI::BufferUsage::Storage, RHI::MemoryType::Upload});
            auto cullFrame = device.CreateBuffer({sizeof(CullFrame), RHI::BufferUsage::Storage, RHI::MemoryType::Upload});
            auto cells = device.CreateBuffer({MaxVisiblePlains * sizeof(CullCell), RHI::BufferUsage::Storage, RHI::MemoryType::Upload});
            auto commandBuffer = device.CreateBuffer({(CounterOffset + ListCount) * sizeof(std::uint32_t),
                                                      RHI::BufferUsage::Storage | RHI::BufferUsage::Indirect, RHI::MemoryType::Upload});
            auto instances = device.CreateBuffer({std::uint64_t{InstanceCount()} * 32, RHI::BufferUsage::Storage});
            auto stats = device.CreateBuffer({ListCount * sizeof(std::uint32_t), RHI::BufferUsage::Storage, RHI::MemoryType::Readback});
            for (const auto* created : {&inputs, &jobs, &cullFrame, &cells, &commandBuffer, &instances, &stats}) {
                if (!*created) {
                    return std::unexpected(created->error());
                }
            }
            frame.inputs = std::move(*inputs);
            frame.jobs = std::move(*jobs);
            frame.cullFrame = std::move(*cullFrame);
            frame.cells = std::move(*cells);
            frame.commands = std::move(*commandBuffer);
            frame.instances = std::move(*instances);
            frame.stats = std::move(*stats);
            frame.sowGroup = device.CreateBindGroup(*renderer->m_sowLayout);
            frame.cullGroup = device.CreateBindGroup(*renderer->m_cullLayout);
            if (!frame.sowGroup || !frame.cullGroup) {
                return std::unexpected("allocation des groupes des arbres");
            }
            frame.sowGroup->SetBuffer(SowInputsSlot, *frame.inputs);
            frame.sowGroup->SetBuffer(SowJobsSlot, *frame.jobs);
            frame.sowGroup->SetBuffer(SowTreesSlot, *renderer->m_trees);
            frame.sowGroup->SetBuffer(SowCountsSlot, *renderer->m_treeCounts);
            frame.cullGroup->SetBuffer(CullFrameSlot, *frame.cullFrame);
            frame.cullGroup->SetBuffer(CullCellsSlot, *frame.cells);
            frame.cullGroup->SetBuffer(CullTreesSlot, *renderer->m_trees);
            frame.cullGroup->SetBuffer(CullCountsSlot, *renderer->m_treeCounts);
            frame.cullGroup->SetBuffer(CullInstancesSlot, *frame.instances);
            frame.cullGroup->SetBuffer(CullCommandsSlot, *frame.commands);
            frame.cullGroup->SetBuffer(CullStatsSlot, *frame.stats);
        }
        return renderer;
    }

    VegetationRenderer::~VegetationRenderer() {
        for (Frame& frame : m_frames) {
            frame.sowGroup.reset(); // avant leur layout
            frame.cullGroup.reset();
        }
    }

    void VegetationRenderer::BeginFrame(std::uint32_t frameIndex) {
        ++m_frame;
        Frame& frame = m_frames[frameIndex];
        if (std::exchange(frame.counted, false)) {
            frame.stats->Read(m_stats.trees.data(), sizeof(m_stats.trees));
        }
        m_entity = NullEntity;
        m_terrain = nullptr;
        m_visible.clear();
    }

    void VegetationRenderer::SetPlanet(Entity entity, const Terrain& terrain, const PlanetComponent& planet,
                                       const VegetationComponent& vegetation, const Math::Quaternionf& rotation,
                                       std::span<const PlanetRenderer::Plain> plains) {
        if (m_entity != NullEntity) {
            return;
        }
        const float relief = std::max(planet.relief, 0.0f);
        const float seaAltitude = planet.seaLevel * 0.01f * relief;
        const float minScale = std::clamp(vegetation.minScale, 0.05f, 20.0f);
        const float maxScale = std::clamp(vegetation.maxScale, minScale, 20.0f);
        const float slope = std::clamp(vegetation.maxSlope, 0.0f, 90.0f) * (std::numbers::pi_v<float> / 180.0f);
        TreeJob job{};
        job.options = {0, static_cast<std::uint32_t>(planet.seed) * 0x9e3779b1u, 0, 0};
        job.placement = {std::clamp(vegetation.density, 0.0f, 1.0f), std::clamp(vegetation.cover, 0.0f, 1.0f), std::cos(slope), 0.0f};
        job.scale = {minScale, maxScale, 0.0f, 0.0f};
        job.shape = {static_cast<float>(terrain.GetRadius()), relief, planet.snowLevel * 0.01f * relief,
                     seaAltitude + std::max(planet.beachHeight, 0.0f)};
        job.climate = {planet.temperature, planet.poleTemperature, std::clamp(planet.humidity, 0.0f, 1.0f),
                       planet.ocean ? static_cast<float>(terrain.GetRadius()) + seaAltitude : 0.0f};
        // Autre forme, autres réglages : chaque plaine est à resemer.
        const std::array<float, 12> signature{job.placement[0], job.placement[1], job.placement[2], job.scale[0], job.scale[1],
                                              job.shape[0], job.shape[1], job.shape[2], job.shape[3], job.climate[0],
                                              job.climate[1], job.climate[2] + job.climate[3] + static_cast<float>(planet.seed)};
        if (entity != m_sownPlanet || signature != m_signature) {
            Forget();
        }
        m_entity = entity;
        m_sownPlanet = entity;
        m_terrain = &terrain;
        m_job = job;
        m_signature = signature;
        m_rotation = rotation;
        m_lodDistance = std::max(vegetation.lodDistance, 0.0f);
        m_farDistance = std::max(vegetation.farDistance, m_lodDistance);
        m_range = std::max(vegetation.range, 0.0f);
        for (const PlanetRenderer::Plain& plain : plains) {
            if (plain.distance < m_range) {
                m_visible.push_back(plain);
            }
        }
    }

    void VegetationRenderer::Forget() {
        for (const auto& [key, plain] : m_stored) {
            m_freeSlots.push_back({plain.slot, m_frame});
        }
        m_stored.clear();
    }

    // Une case libre que le GPU ne lit plus, une nouvelle, ou celle de la plaine servie il y a le plus longtemps (pas
    // à cette image) ; MaxPlains si aucune.
    std::uint32_t VegetationRenderer::TakeSlot() {
        const auto free = std::ranges::find_if(m_freeSlots, [this](const FreeSlot& f) { return f.frame + RHI::FramesInFlight < m_frame; });
        if (free != m_freeSlots.end()) {
            const std::uint32_t slot = free->slot;
            m_freeSlots.erase(free);
            return slot;
        }
        if (m_slotCount < MaxPlains) {
            return m_slotCount++;
        }
        auto oldest = m_stored.end();
        for (auto it = m_stored.begin(); it != m_stored.end(); ++it) {
            if (it->second.lastUsed + RHI::FramesInFlight < m_frame && (oldest == m_stored.end() || it->second.lastUsed < oldest->second.lastUsed)) {
                oldest = it;
            }
        }
        if (oldest == m_stored.end()) {
            return MaxPlains;
        }
        const std::uint32_t slot = oldest->second.slot;
        m_stored.erase(oldest);
        return slot;
    }

    void VegetationRenderer::EndFrame(std::uint32_t frameIndex, const View& view,
                                      const std::array<ShadowCascade, ShadowCascadeCount>& cascades,
                                      const ShadowReceivers& receivers, const Models& models) {
        Frame& frame = m_frames[frameIndex];
        frame.sown = 0;
        frame.culled = 0;
        for (auto& list : m_lists) {
            list.clear();
        }
        m_stats.plains = 0;
        m_stats.pending = 0;
        m_stats.stored = static_cast<std::uint32_t>(m_stored.size());
        if (!m_terrain || m_visible.empty()) {
            return;
        }

        // Plaines en vue : celles déjà semées sont triées ; les autres, semées d'abord les plus proches, quelques-unes
        // par image (elles sont triées dès cette image, après leur semis).
        // Les plus proches seulement, s'il y en a trop.
        std::ranges::sort(m_visible, {}, &PlanetRenderer::Plain::distance);
        if (m_visible.size() > MaxVisiblePlains) {
            m_visible.resize(MaxVisiblePlains);
        }
        m_cells.clear();
        m_jobs.clear();
        for (const PlanetRenderer::Plain& plain : m_visible) {
            const std::uint64_t key = plain.key.Pack();
            auto stored = m_stored.find(key);
            if (stored == m_stored.end()) {
                if (m_jobs.size() >= MaxSownPerFrame) {
                    ++m_stats.pending;
                    continue;
                }
                const std::uint32_t slot = TakeSlot();
                if (slot == MaxPlains) {
                    ++m_stats.pending;
                    continue;
                }
                // Le relief au détail le plus fin : celui du sol sous les pieds.
                PrepareTileGpu(*m_terrain, plain.key, m_inputs);
                m_inputs.options[0] = m_terrain->GetOctaves(m_terrain->GetMaxLevel());
                frame.inputs->Write(&m_inputs, sizeof(m_inputs), m_jobs.size() * sizeof(m_inputs));
                TreeJob job = m_job;
                job.cell = {plain.key.face, plain.key.level, plain.key.x, plain.key.y};
                job.options[0] = slot;
                m_jobs.push_back(job);
                stored = m_stored.emplace(key, StoredPlain{slot, m_frame}).first;
            }
            stored->second.lastUsed = m_frame;
            m_cells.push_back({{static_cast<float>(plain.origin.x), static_cast<float>(plain.origin.y), static_cast<float>(plain.origin.z),
                                std::bit_cast<float>(stored->second.slot)}});
        }
        if (!m_jobs.empty()) {
            frame.jobs->Write(m_jobs.data(), m_jobs.size() * sizeof(TreeJob));
        }
        frame.sown = static_cast<std::uint32_t>(m_jobs.size());
        m_stats.stored = static_cast<std::uint32_t>(m_stored.size());
        if (m_cells.empty() || !models.near || !models.middle || !models.far) {
            return;
        }
        frame.culled = static_cast<std::uint32_t>(m_cells.size());
        m_stats.plains = frame.culled;
        frame.cells->Write(m_cells.data(), m_cells.size() * sizeof(CullCell));

        // Commandes : une par primitive du modèle de chaque liste, à 0 arbre ; le GPU les remplit.
        CullFrame data{};
        m_commands.clear();
        std::uint32_t firstInstance = 0;
        for (std::uint32_t list = 0; list < ListCount; ++list) {
            const GpuModel& model = list == Near ? *models.near : list == Far ? *models.far : *models.middle;
            data.firstInstances[list] = firstInstance;
            data.capacities[list] = Capacities[list];
            for (std::uint32_t p = 0; p < model.primitives.size() && m_commands.size() < MaxCommands; ++p) {
                const ModelPrimitive& primitive = model.primitives[p];
                data.commandLists[m_commands.size()] = list;
                m_lists[list].push_back({&model, p, static_cast<std::uint32_t>(m_commands.size())});
                m_commands.push_back({primitive.indexCount, 0, primitive.firstIndex,
                                      static_cast<std::int32_t>(model.resources.firstVertex), firstInstance});
            }
            firstInstance += Capacities[list];
        }
        std::array<std::uint32_t, CounterOffset + ListCount> words{};
        std::memcpy(words.data(), m_commands.data(), m_commands.size() * sizeof(RHI::DrawIndexedCommand));
        frame.commands->Write(words.data(), sizeof(words));

        const auto vector = [](const Math::Vector3f& v, float w) { return std::array{v.x, v.y, v.z, w}; };
        data.forward = vector(view.forward, view.tanX);
        data.right = vector(view.right, view.tanY);
        data.up = vector(view.up, 1.0f / std::sqrt(1.0f + view.tanX * view.tanX));
        data.distances = {m_lodDistance, m_farDistance, m_range, 1.0f / std::sqrt(1.0f + view.tanY * view.tanY)};
        // Sphère du modèle détaillé : les autres ont la même silhouette.
        const Math::Vector3f half = models.near->bounds.GetSize() * 0.5f;
        data.sphere = vector(models.near->bounds.GetCenter(), Math::Length(half));
        data.rotation = {m_rotation.x, m_rotation.y, m_rotation.z, m_rotation.w};
        for (std::size_t c = 0; c < ShadowCascadeCount; ++c) {
            data.cascades[c] = cascades[c].lightViewProjection;
            // Comme MeshRenderer::CastsShadow : pas plus petit que 1,5 texel de la carte.
            data.cascadeLimits[c] = {1.0f / std::max(cascades[c].radius, 1.0e-3f), 1.0f / std::max(cascades[c].depthRange, 1.0e-3f),
                                     1.5f * cascades[c].texelSize, ShadowCascadeEnds[c]};
        }
        for (std::size_t side = 0; side < 4; ++side) {
            data.receiverSides[side] = vector(receivers.GetSides()[side], 0.0f);
        }
        data.toSun = vector(receivers.GetToSun(), ShadowCasterDistance);
        data.counts = {static_cast<std::uint32_t>(m_commands.size()), frame.culled, 0, 0};
        frame.cullFrame->Write(&data, sizeof(data));
    }

    bool VegetationRenderer::ReadTrees(TileKey key, std::vector<Tree>& trees) const {
        const auto stored = m_stored.find(key.Pack());
        if (stored == m_stored.end()) {
            return false;
        }
        const std::uint32_t slot = stored->second.slot;
        std::uint32_t count = 0;
        m_treeCounts->Read(&count, sizeof(count), slot * sizeof(count));
        trees.resize(std::min(count, TreesPerPlain));
        if (!trees.empty()) {
            m_trees->Read(trees.data(), trees.size() * sizeof(Tree), std::uint64_t{slot} * TreesPerPlain * sizeof(Tree));
        }
        return true;
    }

    void VegetationRenderer::Compute(RHI::CommandList& commands, std::uint32_t frameIndex) {
        Frame& frame = m_frames[frameIndex];
        if (frame.sown > 0) {
            commands.BindPipeline(*m_sow);
            commands.SetBindGroup(0, *frame.sowGroup);
            for (std::uint32_t job = 0; job < frame.sown; ++job) {
                const SowConstants constants{job};
                commands.PushConstants(&constants, sizeof(constants));
                commands.Dispatch(1);
            }
            commands.ComputeBarrier();
        }
        if (frame.culled == 0) {
            return;
        }
        commands.BindPipeline(*m_cull);
        commands.SetBindGroup(0, *frame.cullGroup);
        commands.Dispatch(frame.culled);
        commands.ComputeBarrier();
        commands.BindPipeline(*m_finalize);
        commands.SetBindGroup(0, *frame.cullGroup);
        commands.Dispatch(1);
        commands.ComputeBarrier();
        frame.counted = true;
    }
}
