#include "Renderer/RenderGraph.hpp"

#include <algorithm>
#include <format>

namespace Flux {
    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Reads(ResourceId resource) {
        m_graph.m_passes[m_pass].reads.push_back(resource);
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Writes(ResourceId resource) {
        m_graph.m_passes[m_pass].writes.push_back(resource);
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Begin(Record begin) {
        m_graph.m_passes[m_pass].begin = std::move(begin);
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::When(Condition condition) {
        m_graph.m_passes[m_pass].condition = std::move(condition);
        return *this;
    }

    RenderGraph::PassBuilder& RenderGraph::PassBuilder::Step(const char* name, Record record, Condition condition) {
        m_graph.m_passes[m_pass].steps.push_back({name, std::move(record), std::move(condition)});
        return *this;
    }

    RenderGraph::ResourceId RenderGraph::AddResource(const char* name, Lifetime lifetime) {
        m_resources.push_back({name, lifetime});
        return static_cast<ResourceId>(m_resources.size() - 1);
    }

    RenderGraph::PassBuilder RenderGraph::AddPass(const char* name) {
        m_passes.push_back({name, {}, {}, {}, {}, {}});
        return {*this, static_cast<std::uint32_t>(m_passes.size() - 1)};
    }

    std::expected<void, std::string> RenderGraph::Compile() {
        // Écrite plus tôt dans l'image, ou lisible dès son début.
        std::vector<bool> written(m_resources.size(), false);
        for (std::size_t r = 0; r < m_resources.size(); ++r) {
            written[r] = m_resources[r].lifetime != Lifetime::Frame;
        }
        m_stepCount = 0;
        m_timings.clear();
        for (const Pass& pass : m_passes) {
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
                if (m_resources[resource].lifetime == Lifetime::External) {
                    return std::unexpected(std::format("graphe de rendu : la passe « {} » écrit « {} », remplie hors du graphe",
                                                       pass.name, m_resources[resource].name));
                }
                written[resource] = true;
            }
            m_stepCount += static_cast<std::uint32_t>(pass.steps.size());
            m_timings.push_back({pass.name, -1.0, false});
            if (pass.steps.size() > 1) {
                for (const Step& step : pass.steps) {
                    m_timings.push_back({step.name, -1.0, true});
                }
            }
        }
        if (GetTimestampCount() > RHI::MaxTimestamps) {
            return std::unexpected(std::format("graphe de rendu : {} étapes, au plus {} mesurables", m_stepCount, RHI::MaxTimestamps - 1));
        }
        return {};
    }

    void RenderGraph::Execute(RHI::CommandList& commands, bool timestamps) const {
        std::uint32_t slot = 0;
        if (timestamps) {
            commands.WriteTimestamp(slot);
        }
        for (const Pass& pass : m_passes) {
            const bool active = !pass.condition || pass.condition();
            if (active && pass.begin) {
                pass.begin(commands);
            }
            for (const Step& step : pass.steps) {
                if (active && (!step.condition || step.condition())) {
                    commands.BeginStep(step.name);
                    step.record(commands);
                }
                if (timestamps) {
                    commands.WriteTimestamp(++slot);
                }
            }
            if (active && pass.begin) {
                commands.EndRenderPass();
            }
        }
    }

    std::span<const RenderGraph::Timing> RenderGraph::ReadTimings(std::span<const double> stamps) {
        const auto between = [&](std::uint32_t from, std::uint32_t to) {
            return to < stamps.size() && stamps[from] >= 0.0 && stamps[to] >= 0.0 ? stamps[to] - stamps[from] : -1.0;
        };
        std::uint32_t slot = 0;
        std::size_t row = 0;
        for (const Pass& pass : m_passes) {
            const auto count = static_cast<std::uint32_t>(pass.steps.size());
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
