#include "Renderer/PlanetRenderer.hpp"

#include "Flux/Core/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Flux {
    namespace {
        // Une tuile est coupée en 4 quand la caméra est à moins de SplitFactor fois son bord : ses sommets restent
        // espacés de quelques pixels à l'écran.
        constexpr double SplitFactor = 3.0;
        // Temps de génération des tuiles par image (la première tuile demandée est toujours générée).
        constexpr auto GenerationBudget = std::chrono::microseconds(3000);
        // Nœuds gardés par planète au plus : au-delà, les moins récemment utilisés sont oubliés (environ 40 Ko de
        // sommets par tuile, soit une quarantaine de Mo).
        constexpr std::size_t MaxNodes = 1024;
        // Buffers de sommets libres gardés pour être réutilisés.
        constexpr std::size_t MaxFreeBuffers = 128;
    }

    void PlanetRenderer::BeginFrame() {
        ++m_frame;
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
            it->second.bounds = Planet::GetTileBounds(state.terrain, key);
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
            state.terrain = Planet::Terrain(planet.radius, planet.relief, planet.seed);
            state.radius = planet.radius;
            state.relief = planet.relief;
            state.seed = planet.seed;
        }
        state.lastUsed = m_frame;
        state.requests.clear();

        const Planet::Terrain& terrain = state.terrain;
        const Math::Vector3d camera = rotation.Conjugate().Rotate(-position);
        const double minRadius = std::max(terrain.GetRadius() - terrain.GetRelief(), 1.0);
        const double maxRadius = terrain.GetRadius() + terrain.GetRelief();
        const double distance = Math::Length(camera);
        // Horizon : la caméra voit la sphère du sol le plus bas jusqu'à l'angle acos(rmin / d) depuis le point sous
        // elle, et un sommet (au plus haut) dépasse de acos(rmin / rmax) au-delà.
        const double horizon = distance > minRadius ? std::acos(minRadius / distance) + std::acos(minRadius / maxRadius) : -1.0;
        Visit visit{state, camera, position, rotation, horizon, minRadius, inView, tiles};
        for (std::uint8_t face = 0; face < 6; ++face) {
            const Planet::TileKey key{face, 0, 0, 0};
            Node& node = GetNode(state, key);
            // Les racines sont générées tout de suite : la planète n'apparaît jamais à moitié.
            if (!node.model.vertices && !Generate(state, key, node)) {
                continue;
            }
            VisitNode(visit, key, node);
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
        if (key.level < visit.state.terrain.GetMaxLevel() && distance < SplitFactor * node.bounds.edge) {
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
            visit.tiles.push_back({&node.model, visit.position + visit.rotation.Rotate(node.center), node.radius});
        }
    }

    bool PlanetRenderer::Generate(PlanetState& state, Planet::TileKey key, Node& node) {
        Planet::BuildTile(state.terrain, key, m_mesh);
        constexpr std::uint64_t size = Planet::TileVertexCount * sizeof(ModelVertex);
        std::unique_ptr<RHI::Buffer> buffer;
        // Un buffer libéré que le GPU ne lit plus, sinon un nouveau.
        const auto free = std::ranges::find_if(m_freeBuffers, [this](const FreeBuffer& f) { return f.frame + RHI::FramesInFlight < m_frame; });
        if (free != m_freeBuffers.end()) {
            buffer = std::move(free->buffer);
            m_freeBuffers.erase(free);
        } else {
            auto created = m_device.CreateBuffer({size, RHI::BufferUsage::Vertex, RHI::MemoryType::Upload});
            if (!created) {
                Log::Error("Planète : {}", created.error());
                return false;
            }
            buffer = std::move(*created);
        }
        buffer->Write(m_mesh.vertices.data(), size);
        node.model.vertices = std::move(buffer);
        node.model.indices = m_indices;
        node.model.primitives = {{0, m_indexCount, 0}};
        node.model.materials.resize(1); // blanc : la couleur de la planète vient de la teinte du dessin
        node.model.loaded = true;
        node.center = m_mesh.center;
        node.radius = static_cast<float>(m_mesh.radius);
        return true;
    }

    void PlanetRenderer::Retire(Node& node) {
        if (node.model.vertices) {
            m_freeBuffers.push_back({std::move(node.model.vertices), node.lastUsed});
        }
    }

    void PlanetRenderer::EndFrame() {
        // Tuiles demandées : les plus grossières d'abord (elles débloquent les suivantes), puis les plus proches.
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
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (i > 0 && std::chrono::steady_clock::now() - start > GenerationBudget) {
                break;
            }
            Node& node = GetNode(*pending[i].state, pending[i].request.key);
            if (!node.model.vertices) {
                Generate(*pending[i].state, pending[i].request.key, node);
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
