#include "Renderer/PlanetRenderer.hpp"

#include "Flux/Core/Log.hpp"

#include <algorithm>
#include <cmath>

namespace Flux {
    namespace {
        // Tuiles en attente de génération au plus : les plus urgentes (la file est refaite à chaque image).
        constexpr std::size_t MaxQueuedJobs = 256;
        // Tuiles finies envoyées au GPU par image au plus (une copie de 40 Ko chacune).
        constexpr std::size_t MaxUploadsPerFrame = 32;
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
        for (std::uint8_t face = 0; face < 6; ++face) {
            const Planet::TileKey key{face, 0, 0, 0};
            Node& node = GetNode(state, key);
            // Les racines sont générées tout de suite : la planète n'apparaît jamais à moitié.
            if (!node.model.vertices) {
                Planet::BuildTile(terrain, key, m_mesh);
                if (!Upload(node, m_mesh)) {
                    continue;
                }
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

    // Sommets de la tuile envoyés au GPU.
    bool PlanetRenderer::Upload(Node& node, const Planet::TileMesh& mesh) {
        constexpr std::uint64_t vertexSize = Planet::TileVertexCount * sizeof(ModelVertex);
        constexpr std::uint64_t morphSize = Planet::TileVertexCount * sizeof(Planet::MorphVertex);
        // Des buffers libérés que le GPU ne lit plus, sinon des nouveaux.
        const auto free = std::ranges::find_if(m_freeBuffers, [this](const FreeBuffer& f) { return f.frame + RHI::FramesInFlight < m_frame; });
        if (free != m_freeBuffers.end()) {
            node.model.vertices = std::move(free->vertices);
            node.model.morph = std::move(free->morph);
            m_freeBuffers.erase(free);
        } else {
            auto vertices = m_device.CreateBuffer({vertexSize, RHI::BufferUsage::Vertex, RHI::MemoryType::Upload});
            auto morph = m_device.CreateBuffer({morphSize, RHI::BufferUsage::Vertex, RHI::MemoryType::Upload});
            if (!vertices || !morph) {
                Log::Error("Planète : {}", !vertices ? vertices.error() : morph.error());
                return false;
            }
            node.model.vertices = std::move(*vertices);
            node.model.morph = std::move(*morph);
        }
        node.model.vertices->Write(mesh.vertices.data(), vertexSize);
        node.model.morph->Write(mesh.morph.data(), morphSize);
        node.model.indices = m_indices;
        node.model.primitives = {{0, m_indexCount, 0}};
        node.model.materials.resize(1); // blanc : la couleur de la planète vient de la teinte du dessin
        node.model.loaded = true;
        node.center = mesh.center;
        node.radius = static_cast<float>(mesh.radius);
        node.lowest = mesh.lowest;
        return true;
    }

    void PlanetRenderer::Retire(Node& node) {
        if (node.model.vertices) {
            m_freeBuffers.push_back({std::move(node.model.vertices), std::move(node.model.morph), node.lastUsed});
        }
    }

    void PlanetRenderer::EndFrame() {
        // Tuiles finies : envoyées au GPU, sauf celles qui ne servent plus (nœud oublié, planète changée).
        m_results.clear();
        m_generator->Collect(m_results, MaxUploadsPerFrame);
        for (const Planet::TileGenerator::Result& result : m_results) {
            PlanetState* state = FindOwner(result.owner);
            if (!state) {
                continue;
            }
            const auto node = state->nodes.find(result.key.Pack());
            if (node != state->nodes.end() && node->second.queued) {
                node->second.queued = false;
                if (!node->second.model.vertices) {
                    Upload(node->second, result.mesh);
                }
            }
        }

        // Tâches pas encore commencées : reprises, puis la file refaite avec les demandes de cette image.
        m_jobs.clear();
        m_generator->TakeWaiting(m_jobs);
        for (const Planet::TileGenerator::Job& job : m_jobs) {
            if (PlanetState* state = FindOwner(job.owner)) {
                if (const auto node = state->nodes.find(job.key.Pack()); node != state->nodes.end()) {
                    node->second.queued = false;
                }
            }
        }
        m_jobs.clear();
        // Les plus grossières d'abord (elles débloquent les suivantes), puis les plus proches.
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
            if (m_jobs.size() >= MaxQueuedJobs) {
                break;
            }
            Node& node = GetNode(*item.state, item.request.key);
            if (!node.model.vertices && !node.queued) {
                node.queued = true;
                m_jobs.push_back({item.state->terrain, item.state->owner, item.request.key});
            }
        }
        m_generator->Submit(m_jobs);

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
