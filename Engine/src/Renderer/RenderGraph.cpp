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
                auto created = device.CreateRenderTarget(resource.desc->format, extent, resource.desc->sampled);
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
        for (const Resource& resource : m_resources) {
            if (resource.desc && resource.lifetime == Lifetime::External) {
                return std::unexpected(std::format("graphe de rendu : « {} » est créée par le graphe, pas remplie hors de lui",
                                                   resource.name));
            }
        }
        std::size_t uses = 0;
        for (const Pass& pass : m_passes) {
            uses = std::max(uses, pass.reads.size() + pass.writes.size());
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
            }
            m_stepCount += static_cast<std::uint32_t>(pass.steps.size());
            m_timings.push_back({pass.name, -1.0, false});
            if (pass.steps.size() > 1) {
                for (const Step& step : pass.steps) {
                    m_timings.push_back({step.name, -1.0, true});
                }
            }
        }
        m_uses.reserve(uses);
        if (GetTimestampCount() > RHI::MaxTimestamps) {
            return std::unexpected(std::format("graphe de rendu : {} étapes, au plus {} mesurables", m_stepCount, RHI::MaxTimestamps - 1));
        }
        return {};
    }

    void RenderGraph::Execute(RHI::CommandList& commands, bool timestamps) {
        std::uint32_t slot = 0;
        if (timestamps) {
            commands.WriteTimestamp(slot);
        }
        for (const Pass& pass : m_passes) {
            const bool active = !pass.condition || pass.condition();
            if (active) {
                PrepareTextures(commands, pass);
            }
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
            if (active) {
                // L'exemplaire écrit devient celui que liront les passes suivantes, et l'image d'après.
                for (const ResourceId resource : pass.writes) {
                    if (Resource& r = m_resources[resource]; r.IsHistory()) {
                        r.written = 1 - r.written;
                    }
                }
            }
        }
    }

    void RenderGraph::PrepareTextures(RHI::CommandList& commands, const Pass& pass) {
        m_uses.clear(); // capacité gardée : pas d'allocation
        for (const ResourceId resource : pass.reads) {
            if (const RHI::Texture* texture = ReadTexture(m_resources[resource])) {
                m_uses.push_back({texture, RHI::TextureAccess::Sampled, false});
            }
        }
        // Contenu jeté : texture de l'image, ou exemplaire d'historique qui ne tient que l'avant-dernière image.
        for (const ResourceId resource : pass.writes) {
            const Resource& r = m_resources[resource];
            if (const RHI::Texture* texture = WriteTexture(r)) {
                const bool depth = texture->GetFormat() == RHI::Format::D32Float;
                m_uses.push_back({texture, depth ? RHI::TextureAccess::DepthTarget : RHI::TextureAccess::ColorTarget,
                                  r.lifetime == Lifetime::Frame || r.IsHistory()});
            }
        }
        if (!m_uses.empty()) {
            commands.UseTextures(m_uses);
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
