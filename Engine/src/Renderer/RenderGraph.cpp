#include "Renderer/RenderGraph.hpp"

#include "Flux/Core/JobSystem.hpp"

#include <algorithm>
#include <chrono>
#include <format>

namespace Flux {
    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Reads(ResourceId resource) {
        m_graph.m_passes[m_pass].reads.push_back(resource);
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Writes(ResourceId resource) {
        return Writes(resource, RHI::TextureAccess::None);
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Writes(ResourceId resource, RHI::TextureAccess access) {
        m_graph.m_passes[m_pass].writes.push_back(resource);
        m_graph.m_passes[m_pass].writeAccess.push_back(access);
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Begin(Record begin) {
        m_graph.m_passes[m_pass].begin = std::move(begin);
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::BeginParallel(std::function<RHI::RenderPassDesc()> desc, Record setup) {
        m_graph.m_passes[m_pass].parallelDesc = std::move(desc);
        m_graph.m_passes[m_pass].setup = std::move(setup);
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::When(Condition condition) {
        m_graph.m_passes[m_pass].condition = std::move(condition);
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Prepare(std::function<void()> prepare) {
        m_graph.m_passes[m_pass].prepare = std::move(prepare);
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Step(const char* name, Record record, Condition condition) {
        m_graph.m_passes[m_pass].steps.push_back({name, std::move(record), std::move(condition)});
        return *this;
    }

    RenderGraph::ResourceId RenderGraph::AddResource(const char* name, Lifetime lifetime) {
        m_resources.push_back({name, lifetime, {}, {}});
        return static_cast<ResourceId>(m_resources.size() - 1);
    }

    RenderGraph::ResourceId RenderGraph::AddTexture(const char* name, Lifetime lifetime, TextureSource texture) {
        m_resources.push_back({name, lifetime, std::move(texture)});
        return static_cast<ResourceId>(m_resources.size() - 1);
    }

    RenderGraph::ResourceId RenderGraph::CreateTexture(const char* name, Lifetime lifetime, TextureDesc desc) {
        m_resources.push_back({name, lifetime, {}, desc});
        return static_cast<ResourceId>(m_resources.size() - 1);
    }

    std::expected<bool, std::string> RenderGraph::ResizeTextures(RHI::Device& device, TextureSize size, RHI::Extent extent) {
        extent = {std::max(extent.width, 1u), std::max(extent.height, 1u)};
        const auto stale = [&](const Resource& resource) {
            return resource.desc && resource.desc->size == size && (!resource.owned[0] || resource.extent != extent);
        };
        if (std::ranges::none_of(m_resources, stale)) {
            return false;
        }
        device.WaitIdle(); // les images en vol les utilisent peut-être encore
        for (Resource& resource : m_resources) {
            if (!stale(resource)) {
                continue;
            }
            for (auto& texture : resource.owned) {
                texture.reset(); // tout libéré avant d'allouer à la nouvelle taille
            }
            resource.extent = extent;
            resource.written = 0;
        }
        for (Resource& resource : m_resources) {
            if (!resource.desc || resource.desc->size != size || resource.owned[0]) {
                continue;
            }
            for (std::uint32_t copy = 0; copy < (resource.IsHistory() ? 2u : 1u); ++copy) {
                auto created = device.CreateRenderTarget(resource.desc->format, extent, resource.desc->sampled, resource.desc->storage);
                if (!created) {
                    return std::unexpected(std::format("graphe de rendu : « {} » : {}", resource.name, created.error()));
                }
                resource.owned[copy] = std::move(*created);
            }
        }
        return true;
    }

    const RHI::Texture* RenderGraph::GetTexture(ResourceId resource) const {
        return ReadTexture(m_resources[resource]);
    }

    const RHI::Texture* RenderGraph::GetCopy(ResourceId resource, std::uint32_t copy) const {
        return m_resources[resource].owned[copy].get();
    }

    std::uint32_t RenderGraph::GetWriteCopy(ResourceId resource) const {
        const Resource& r = m_resources[resource];
        return r.IsHistory() ? 1 - r.written : 0;
    }

    const RHI::Texture* RenderGraph::ReadTexture(const Resource& resource) const {
        if (resource.desc) {
            return resource.owned[resource.written].get();
        }
        return resource.texture ? resource.texture() : nullptr;
    }

    const RHI::Texture* RenderGraph::WriteTexture(const Resource& resource) const {
        if (resource.desc) {
            return resource.owned[resource.IsHistory() ? 1 - resource.written : 0].get();
        }
        return resource.texture ? resource.texture() : nullptr;
    }

    RenderGraph::PassBuilder RenderGraph::AddPass(const char* name) {
        m_passes.push_back({.name = name});
        return {*this, static_cast<std::uint32_t>(m_passes.size() - 1)};
    }

    std::expected<void, std::string> RenderGraph::Compile() {
        // Écrite plus tôt dans l'image, ou lisible dès son début.
        std::vector<bool> written(m_resources.size(), false);
        for (std::size_t r = 0; r < m_resources.size(); ++r) {
            written[r] = m_resources[r].lifetime != Lifetime::Frame;
        }
        std::vector<bool> drawn(m_resources.size(), false); // texture de l'image déjà écrite par une passe
        m_stepCount = 0;
        m_timings.clear();
        for (const Resource& resource : m_resources) {
            if (resource.desc && resource.lifetime == Lifetime::External) {
                return std::unexpected(std::format("graphe de rendu : « {} » est créée par le graphe, pas remplie hors de lui",
                                                   resource.name));
            }
        }
        std::size_t uses = 0;
        for (Pass& pass : m_passes) {
            pass.firstUse = static_cast<std::uint32_t>(uses);
            pass.firstSlot = m_stepCount + 1; // case 0 : le début de l'image
            uses += pass.reads.size() + pass.writes.size();
            pass.continued.clear();
            if (pass.steps.empty()) {
                return std::unexpected(std::format("graphe de rendu : la passe « {} » n'a aucune étape", pass.name));
            }
            for (const ResourceId resource : pass.reads) {
                if (resource >= m_resources.size()) {
                    return std::unexpected(std::format("graphe de rendu : la passe « {} » lit une ressource inconnue", pass.name));
                }
                if (!written[resource]) {
                    return std::unexpected(std::format("graphe de rendu : la passe « {} » lit « {} » avant qu'une passe l'écrive",
                                                       pass.name, m_resources[resource].name));
                }
            }
            for (const ResourceId resource : pass.writes) {
                if (resource >= m_resources.size()) {
                    return std::unexpected(std::format("graphe de rendu : la passe « {} » écrit une ressource inconnue", pass.name));
                }
                if (m_resources[resource].IsTexture() && !m_resources[resource].IsHistory()
                    && std::ranges::contains(pass.reads, resource)) {
                    return std::unexpected(std::format("graphe de rendu : la passe « {} » lit et écrit la texture « {} »",
                                                       pass.name, m_resources[resource].name));
                }
                if (m_resources[resource].lifetime == Lifetime::External) {
                    return std::unexpected(std::format("graphe de rendu : la passe « {} » écrit « {} », remplie hors du graphe",
                                                       pass.name, m_resources[resource].name));
                }
                written[resource] = true;
                if (drawn[resource]) {
                    pass.continued.push_back(resource);
                }
                drawn[resource] = true;
            }
            m_stepCount += static_cast<std::uint32_t>(pass.steps.size());
            m_timings.push_back({pass.name, -1.0, false, -1.0});
            if (pass.steps.size() > 1) {
                for (const Step& step : pass.steps) {
                    m_timings.push_back({step.name, -1.0, true, -1.0});
                }
            }
        }
        m_planned.assign(uses, {});
        if (GetTimestampCount() > RHI::MaxTimestamps) {
            return std::unexpected(std::format("graphe de rendu : {} étapes, au plus {} mesurables", m_stepCount, RHI::MaxTimestamps - 1));
        }
        return {};
    }

    void RenderGraph::SetParallelism(std::uint32_t maxLists, double listMicroseconds) {
        m_maxLists = std::clamp(maxLists, 1u, MaxLists);
        m_listMicroseconds = listMicroseconds;
    }

    std::uint32_t RenderGraph::CollectUses(const Pass& pass, RHI::TextureUse* uses) const {
        std::uint32_t count = 0;
        for (const ResourceId resource : pass.reads) {
            if (const RHI::Texture* texture = ReadTexture(m_resources[resource])) {
                uses[count++] = {texture, RHI::TextureAccess::Sampled, false, {}};
            }
        }
        // Contenu jeté : texture de l'image à sa première écriture, ou exemplaire d'historique qui ne tient que
        // l'avant-dernière image.
        for (std::size_t w = 0; w < pass.writes.size(); ++w) {
            const ResourceId resource = pass.writes[w];
            const Resource& r = m_resources[resource];
            if (const RHI::Texture* texture = WriteTexture(r)) {
                const bool depth = texture->GetFormat() == RHI::Format::D32Float;
                const bool first = r.lifetime == Lifetime::Frame && !std::ranges::contains(pass.continued, resource);
                const RHI::TextureAccess access = pass.writeAccess[w] != RHI::TextureAccess::None ? pass.writeAccess[w]
                                                : depth ? RHI::TextureAccess::DepthTarget
                                                        : RHI::TextureAccess::ColorTarget;
                uses[count++] = {texture, access, first || r.IsHistory(), {}};
            }
        }
        return count;
    }

    void RenderGraph::Plan(RHI::CommandList& commands) {
        for (Pass& pass : m_passes) {
            pass.active = !pass.condition || pass.condition();
            pass.useCount = 0;
            for (Step& step : pass.steps) {
                step.active = pass.active && (!step.condition || step.condition());
            }
            if (!pass.active) {
                continue;
            }
            if (pass.prepare) {
                pass.prepare();
            }
            RHI::TextureUse* uses = m_planned.data() + pass.firstUse;
            pass.useCount = CollectUses(pass, uses);
            if (pass.useCount > 0) {
                commands.PlanTextures({uses, pass.useCount});
            }
            // L'exemplaire écrit devient celui que liront les passes suivantes, et l'image d'après.
            for (const ResourceId resource : pass.writes) {
                if (Resource& r = m_resources[resource]; r.IsHistory()) {
                    r.written = 1 - r.written;
                }
            }
        }
    }

    double RenderGraph::GetSerialMicroseconds() const {
        double total = 0.0;
        for (const Pass& pass : m_passes) {
            total += pass.active ? pass.cpuMicroseconds : 0.0;
        }
        return total;
    }

    std::uint32_t RenderGraph::ChooseGroups() {
        const auto count = static_cast<std::uint32_t>(m_passes.size());
        // Une passe jouée compte au moins une microseconde : son premier enregistrement n'est pas encore mesuré.
        const auto cost = [](const Pass& pass) { return pass.active ? std::max(pass.cpuMicroseconds, 1.0) : 0.0; };
        double total = 0.0;
        for (const Pass& pass : m_passes) {
            total += cost(pass);
        }
        const double wanted = m_listMicroseconds > 0.0 ? total / m_listMicroseconds : static_cast<double>(m_maxLists);
        const std::uint32_t groups = std::min({m_maxLists, count, static_cast<std::uint32_t>(std::max(wanted, 1.0))});
        if (groups < 2) {
            return 1;
        }
        // Coupé dès qu'un groupe atteint sa part du total, en laissant au moins une passe à chaque groupe suivant.
        std::uint32_t group = 1;
        double sum = 0.0;
        m_bounds[0] = 0;
        for (std::uint32_t i = 0; i + 1 < count && group < groups; ++i) {
            sum += cost(m_passes[i]);
            if (sum >= total * group / groups || count - (i + 1) == groups - group) {
                m_bounds[group++] = i + 1;
            }
        }
        m_bounds[group] = count;
        return group;
    }

    void RenderGraph::RecordPasses(RHI::CommandList& commands, std::uint32_t first, std::uint32_t last, bool timestamps) {
        using Clock = std::chrono::steady_clock;
        for (std::uint32_t p = first; p < last; ++p) {
            Pass& pass = m_passes[p];
            const Clock::time_point start = Clock::now();
            if (pass.useCount > 0) {
                commands.UseTextures({m_planned.data() + pass.firstUse, pass.useCount});
            }
            if (pass.active && pass.parallelDesc) {
                RecordParallelSteps(commands, pass, timestamps);
            } else {
                if (pass.active && pass.begin) {
                    pass.begin(commands);
                }
                std::uint32_t slot = pass.firstSlot;
                for (const Step& step : pass.steps) {
                    RecordStep(commands, step, slot++, timestamps);
                }
                if (pass.active && pass.begin) {
                    commands.EndRenderPass();
                }
            }
            const double spent = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
            pass.cpuMicroseconds = pass.cpuMicroseconds == 0.0 ? spent : pass.cpuMicroseconds * 0.9 + spent * 0.1;
        }
    }

    void RenderGraph::RecordStep(RHI::CommandList& commands, const Step& step, std::uint32_t slot, bool timestamps) {
        if (step.active) {
            commands.BeginStep(step.name);
            step.record(commands);
        }
        if (timestamps) {
            commands.WriteTimestamp(slot);
        }
    }

    void RenderGraph::RecordParallelSteps(RHI::CommandList& commands, const Pass& pass, bool timestamps) {
        const auto count = static_cast<std::uint32_t>(pass.steps.size());
        const RHI::RenderPassDesc desc = pass.parallelDesc();
        const std::span<RHI::CommandList* const> lists = m_jobs ? commands.BeginParallelRenderPass(desc, count)
                                                                 : std::span<RHI::CommandList* const>{};
        if (lists.size() == count) {
            // Une étape par liste : chacune ne touche que la sienne (heure comprise, notée à la fin de l'étape).
            m_jobs->ParallelFor(count, 1, [&pass, lists, timestamps](std::uint32_t begin, std::uint32_t end) {
                for (std::uint32_t i = begin; i < end; ++i) {
                    if (pass.setup) {
                        pass.setup(*lists[i]);
                    }
                    RecordStep(*lists[i], pass.steps[i], pass.firstSlot + i, timestamps);
                }
            });
        } else {
            if (!m_jobs) {
                commands.BeginRenderPass(desc);
            }
            if (pass.setup) {
                pass.setup(commands);
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                RecordStep(commands, pass.steps[i], pass.firstSlot + i, timestamps);
            }
        }
        commands.EndRenderPass();
    }

    void RenderGraph::Execute(RHI::CommandList& commands, bool timestamps, JobSystem* jobs) {
        m_jobs = jobs;
        if (timestamps) {
            commands.WriteTimestamp(0);
        }
        Plan(commands);
        const auto count = static_cast<std::uint32_t>(m_passes.size());
        m_listCount = 1;
        const std::uint32_t groups = jobs ? ChooseGroups() : 1;
        if (groups > 1) {
            const std::span<RHI::CommandList* const> lists = commands.Fork(groups);
            if (lists.size() == groups) {
                // Le fil d'Execute fait le premier groupe ; chaque passe n'est touchée que par le fil de son groupe.
                jobs->ParallelFor(groups, 1, [this, lists, timestamps](std::uint32_t begin, std::uint32_t end) {
                    for (std::uint32_t g = begin; g < end; ++g) {
                        RecordPasses(*lists[g], m_bounds[g], m_bounds[g + 1], timestamps);
                    }
                });
                m_listCount = groups;
                return;
            }
        }
        RecordPasses(commands, 0, count, timestamps);
    }

    std::span<const RenderGraph::Timing> RenderGraph::ReadTimings(std::span<const double> stamps) {
        const auto between = [&](std::uint32_t from, std::uint32_t to) {
            return to < stamps.size() && stamps[from] >= 0.0 && stamps[to] >= 0.0 ? stamps[to] - stamps[from] : -1.0;
        };
        std::uint32_t slot = 0;
        std::size_t row = 0;
        for (const Pass& pass : m_passes) {
            const auto count = static_cast<std::uint32_t>(pass.steps.size());
            m_timings[row].cpuMilliseconds = pass.cpuMicroseconds / 1000.0;
            m_timings[row++].milliseconds = between(slot, slot + count);
            if (count > 1) {
                for (std::uint32_t s = 0; s < count; ++s) {
                    m_timings[row++].milliseconds = between(slot + s, slot + s + 1);
                }
            }
            slot += count;
        }
        return m_timings;
    }
}
