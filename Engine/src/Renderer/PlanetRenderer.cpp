#include "Renderer/PlanetRenderer.hpp"

#include "Flux/Core/Log.hpp"

#include <algorithm>
#include <cmath>

namespace Flux {
    namespace {
        // Nœuds gardés par planète au plus : au-delà, les moins récemment utilisés sont oubliés (environ 70 Ko de
        // sommets par tuile, soit environ 70 Mo de mémoire vidéo).
        constexpr std::size_t MaxNodes = 1024;
        // Buffers de sommets libres gardés pour être réutilisés.
        constexpr std::size_t MaxFreeBuffers = 128;
    }

    std::expected<std::unique_ptr<PlanetRenderer>, std::string> PlanetRenderer::Create(RHI::Device& device) {
        auto compute = PlanetTileCompute::Create(device);
        if (!compute) {
            return std::unexpected(compute.error());
        }
        return std::unique_ptr<PlanetRenderer>(new PlanetRenderer(device, std::move(*compute)));
    }

    void PlanetRenderer::BeginFrame(std::uint32_t frameIndex) {
        ++m_frame;
        // Mesures des tuiles calculées avec cette image (le GPU l'a finie) : leur vraie sphère englobante et leur
        // point le plus bas remplacent les estimations prudentes.
        const auto stats = m_compute->GetStats(frameIndex);
        const std::vector<Computed>& computed = m_computed[frameIndex];
        for (std::size_t i = 0; i < std::min(stats.size(), computed.size()); ++i) {
            if (PlanetState* state = FindOwner(computed[i].owner)) {
                if (const auto node = state->nodes.find(computed[i].key); node != state->nodes.end() && node->second.model.vertices) {
                    node->second.radius = stats[i].radius;
                    node->second.lowest = stats[i].lowest;
                }
            }
        }
        m_computed[frameIndex].clear();
        if (!m_indices) {
            const std::vector<std::uint32_t> indices = Planet::BuildTileIndices();
            auto buffer = m_device.CreateBuffer({indices.size() * sizeof(std::uint32_t), RHI::BufferUsage::Index}, indices.data());
            if (!buffer) {
                Log::Error("Planète : {}", buffer.error());
                return;
            }
            m_indices = std::move(*buffer);
            m_indexCount = static_cast<std::uint32_t>(indices.size());
        }
    }

    PlanetRenderer::Node& PlanetRenderer::GetNode(PlanetState& state, Planet::TileKey key) {
        auto [it, created] = state.nodes.try_emplace(key.Pack());
        if (created) {
            it->second.bounds = Planet::GetTileBounds(*state.terrain, key);
        }
        return it->second;
    }

    void PlanetRenderer::Collect(Entity entity, const PlanetComponent& planet, const Math::Vector3d& position,
                                 const Math::Quaternionf& rotation, const ViewTest& inView, std::vector<Tile>& tiles) {
        if (!m_indices) {
            return;
        }
        PlanetState& state = m_planets[entity];
        if (planet.radius != state.radius || planet.relief != state.relief || planet.seed != state.seed) {
            // Autre forme : toutes les tuiles sont à refaire.
            for (auto& [key, node] : state.nodes) {
                Retire(node);
            }
            state.nodes.clear();
            state.terrain = std::make_shared<const Planet::Terrain>(planet.radius, planet.relief, planet.seed);
            state.owner = m_nextOwner++; // les tuiles de l'ancienne forme encore en cours seront ignorées
            state.radius = planet.radius;
            state.relief = planet.relief;
            state.seed = planet.seed;
        }
        state.lastUsed = m_frame;
        state.requests.clear();

        const Planet::Terrain& terrain = *state.terrain;
        const Math::Vector3d camera = rotation.Conjugate().Rotate(-position);
        const double minRadius = std::max(terrain.GetRadius() - terrain.GetRelief(), 1.0);
        const double maxRadius = terrain.GetRadius() + terrain.GetRelief();
        const double distance = Math::Length(camera);
        // Horizon : la caméra voit la sphère du sol le plus bas jusqu'à l'angle acos(rmin / d) depuis le point sous
        // elle, et un sommet (au plus haut) dépasse de acos(rmin / rmax) au-delà.
        const double horizon = distance > minRadius ? std::acos(minRadius / distance) + std::acos(minRadius / maxRadius) : -1.0;
        Visit visit{state, camera, position, rotation, horizon, minRadius, inView, tiles};
        // Les racines d'abord : la planète apparaît d'un coup, quand ses six racines sont calculées (la même image).
        bool rootsReady = true;
        for (std::uint8_t face = 0; face < 6; ++face) {
            Node& node = GetNode(state, {face, 0, 0, 0});
            node.lastUsed = m_frame;
            if (!node.model.vertices) {
                rootsReady = false;
                state.requests.push_back({{face, 0, 0, 0}, 0.0});
            }
        }
        if (!rootsReady) {
            return;
        }
        for (std::uint8_t face = 0; face < 6; ++face) {
            const Planet::TileKey key{face, 0, 0, 0};
            VisitNode(visit, key, GetNode(state, key));
        }
    }

    bool PlanetRenderer::IsCulled(const Visit& visit, const Node& node) const {
        const Planet::TileBounds& bounds = node.bounds;
        const double centerLength = Math::Length(bounds.center);
        if (visit.horizon >= 0.0 && centerLength > 0.0) {
            const double cosine = Math::Dot(visit.camera, bounds.center) / (Math::Length(visit.camera) * centerLength);
            const double angle = std::acos(std::clamp(cosine, -1.0, 1.0));
            const double angularRadius = std::asin(std::min(bounds.radius / centerLength, 1.0));
            if (angle - angularRadius > visit.horizon) {
                return true;
            }
        }
        const Math::Vector3d relative = visit.position + visit.rotation.Rotate(bounds.center);
        return !visit.inView(Math::Vector3f(relative), static_cast<float>(bounds.radius));
    }

    void PlanetRenderer::VisitNode(Visit& visit, Planet::TileKey key, Node& node) {
        node.lastUsed = m_frame;
        if (IsCulled(visit, node)) {
            return;
        }
        const double distance = Math::Length(visit.camera - node.bounds.center) - node.bounds.radius;
        const Planet::Terrain& terrain = *visit.state.terrain;
        if (key.level < terrain.GetMaxLevel() && distance < Planet::GetSplitDistance(terrain, key.level)) {
            // Les enfants visibles doivent tous être prêts ; sinon cette tuile reste affichée et ils sont demandés.
            std::array<Node*, 4> children{};
            std::array<bool, 4> culled{};
            bool ready = true;
            for (std::uint32_t c = 0; c < 4; ++c) {
                const Planet::TileKey childKey = key.Child(c);
                children[c] = &GetNode(visit.state, childKey);
                children[c]->lastUsed = m_frame;
                culled[c] = IsCulled(visit, *children[c]);
                if (!culled[c] && !children[c]->model.vertices) {
                    ready = false;
                    const double childDistance = Math::Length(visit.camera - children[c]->bounds.center) - children[c]->bounds.radius;
                    visit.state.requests.push_back({childKey, std::max(childDistance, 0.0)});
                }
            }
            if (ready) {
                for (std::uint32_t c = 0; c < 4; ++c) {
                    if (!culled[c]) {
                        VisitNode(visit, key.Child(c), *children[c]);
                    }
                }
                return;
            }
        }
        if (node.model.vertices) {
            // Glisse vers la forme parente jusqu'à la distance où la parente reprend la place (une voisine plus
            // grossière n'est jamais plus proche) : là, son bord est celui de la parente, sans fente.
            float morphStart = 1.0e30f;
            float morphScale = 0.0f;
            if (key.level > 0) {
                const double end = Planet::GetSplitDistance(terrain, key.level - 1u);
                morphStart = static_cast<float>(Planet::MorphStart * end);
                morphScale = static_cast<float>(1.0 / ((1.0 - Planet::MorphStart) * end));
            }
            visit.tiles.push_back({&node.model, visit.position + visit.rotation.Rotate(node.center), node.radius, morphStart, morphScale,
                                   node.lowest});
        }
    }

    PlanetRenderer::PlanetState* PlanetRenderer::FindOwner(std::uint64_t owner) {
        for (auto& [entity, state] : m_planets) {
            if (state.owner == owner) {
                return &state;
            }
        }
        return nullptr;
    }

    // Tuile confiée au GPU, calculée au début de l'image suivante (PlanetRenderer::Compute) : ses buffers sont prêts
    // pour tout dessin qui suit. false si l'image est pleine.
    bool PlanetRenderer::Schedule(PlanetState& state, Planet::TileKey key, Node& node, std::uint32_t frameIndex) {
        // Des buffers libérés que le GPU n'utilise plus, sinon des nouveaux.
        PlanetTileCompute::TileBuffers buffers;
        const auto free = std::ranges::find_if(m_freeBuffers, [this](const FreeBuffer& f) { return f.frame + RHI::FramesInFlight < m_frame; });
        if (free != m_freeBuffers.end()) {
            buffers = std::move(free->buffers);
            m_freeBuffers.erase(free);
        } else {
            auto created = m_compute->CreateTileBuffers();
            if (!created) {
                Log::Error("Planète : {}", created.error());
                return false;
            }
            buffers = std::move(*created);
        }
        if (!m_compute->Add(frameIndex, *state.terrain, key, buffers)) {
            m_freeBuffers.push_back({std::move(buffers), 0});
            return false;
        }
        m_computed[frameIndex].push_back({state.owner, key.Pack()});
        node.model.vertices = std::move(buffers.vertices);
        node.model.morph = std::move(buffers.morph);
        node.group = std::move(buffers.group);
        node.model.indices = m_indices;
        node.model.primitives = {{0, m_indexCount, 0}};
        node.model.materials.resize(1); // blanc : la couleur de la planète vient de la teinte du dessin
        node.model.loaded = true;
        // Le centre de la tuile est celui de sa sphère (Planet::GetTileBounds le calcule pareil) ; rayon et point le
        // plus bas prudents jusqu'au retour des mesures du GPU.
        node.center = node.bounds.center;
        node.radius = static_cast<float>(node.bounds.radius);
        node.lowest = state.terrain->GetRadius() - 1.5 * state.terrain->GetRelief() - node.bounds.edge;
        node.lastUsed = m_frame;
        return true;
    }

    void PlanetRenderer::Retire(Node& node) {
        if (node.model.vertices) {
            m_freeBuffers.push_back({{std::move(node.model.vertices), std::move(node.model.morph), std::move(node.group)}, node.lastUsed});
        }
    }

    void PlanetRenderer::EndFrame(std::uint32_t frameIndex) {
        // Tuiles demandées : les plus grossières d'abord (elles débloquent les suivantes), puis les plus proches, autant
        // que le GPU en calcule par image.
        struct Pending
        {
            PlanetState* state;
            Request request;
        };
        std::vector<Pending> pending;
        for (auto& [entity, state] : m_planets) {
            for (const Request& request : state.requests) {
                pending.push_back({&state, request});
            }
        }
        std::ranges::sort(pending, [](const Pending& a, const Pending& b) {
            return a.request.key.level != b.request.key.level ? a.request.key.level < b.request.key.level
                                                              : a.request.distance < b.request.distance;
        });
        for (const Pending& item : pending) {
            Node& node = GetNode(*item.state, item.request.key);
            if (!node.model.vertices && !Schedule(*item.state, item.request.key, node, frameIndex)) {
                break;
            }
        }

        // Planètes disparues de la scène (ou plus dessinées) : leurs tuiles sont libérées.
        for (auto it = m_planets.begin(); it != m_planets.end();) {
            if (it->second.lastUsed != m_frame) {
                for (auto& [key, node] : it->second.nodes) {
                    Retire(node);
                }
                it = m_planets.erase(it);
            } else {
                ++it;
            }
        }
        // Trop de nœuds : les moins récemment visités (jamais ceux de cette image, ni les racines) sont oubliés.
        for (auto& [entity, state] : m_planets) {
            if (state.nodes.size() <= MaxNodes) {
                continue;
            }
            std::vector<std::pair<std::uint64_t, std::uint64_t>> old; // dernière visite, clé
            for (const auto& [key, node] : state.nodes) {
                if (node.lastUsed != m_frame && (key >> 59) != 0) {
                    old.emplace_back(node.lastUsed, key);
                }
            }
            const std::size_t excess = std::min(state.nodes.size() - MaxNodes, old.size());
            std::ranges::nth_element(old, old.begin() + static_cast<std::ptrdiff_t>(excess));
            for (std::size_t i = 0; i < excess; ++i) {
                auto node = state.nodes.find(old[i].second);
                Retire(node->second);
                state.nodes.erase(node);
            }
        }
        // Trop de buffers libres : ceux que le GPU ne lit plus sont rendus.
        std::size_t count = m_freeBuffers.size();
        std::erase_if(m_freeBuffers, [&](const FreeBuffer& f) {
            if (count <= MaxFreeBuffers || f.frame + RHI::FramesInFlight >= m_frame) {
                return false;
            }
            --count;
            return true;
        });
    }
}
