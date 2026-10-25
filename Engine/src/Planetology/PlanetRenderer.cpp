#include "Planetology/PlanetRenderer.hpp"

#include "Flux/Core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Flux::Planetology {
    namespace {
        // Nœuds gardés par planète au plus, au détail Bas : au-delà, les moins récemment utilisés sont oubliés
        // (environ 70 Ko de sommets par tuile, soit environ 70 Mo de mémoire vidéo). Autant de fois plus que de
        // tuiles en plus aux détails plus fins (carré du facteur de coupe).
        constexpr std::size_t MaxNodes = 1024;
        // Les tuiles sont demandées en avance pour là où la caméra sera dans ce temps-là (s) : à pied, quelques mètres,
        // rien ne change ; en vaisseau, des kilomètres, les tuiles sont prêtes quand il arrive. Elles ne sont dessinées
        // qu'à leur distance : le détail à l'écran ne dépend pas de la vitesse (sinon il retombe à l'arrêt).
        constexpr double LookAhead = 2.0;
        // Au-delà (m/s), la caméra a sauté (téléportation, autre scène) : pas de vitesse.
        constexpr double MaxCameraSpeed = 100000.0;

        // Distance de `point` au trajet de la caméra, de `from` à `to`.
        double DistanceToPath(const Math::Vector3d& from, const Math::Vector3d& to, const Math::Vector3d& point) {
            const Math::Vector3d path = to - from;
            const double length2 = Math::Dot(path, path);
            const double t = length2 > 0.0 ? std::clamp(Math::Dot(point - from, path) / length2, 0.0, 1.0) : 0.0;
            return Math::Length(from + path * t - point);
        }
    }

    bool PlanetRenderer::IsBehindHorizon(const Math::Vector3d& camera, const Math::Vector3d& center, double radius,
                                         double occluder, double top) {
        const double distance = Math::Length(camera);
        const double centerLength = Math::Length(center);
        if (distance <= occluder || centerLength <= 0.0) {
            return false;
        }
        // La caméra voit la sphère `occluder` jusqu'à l'angle acos(occluder / d) depuis le point sous elle ; un point
        // au rayon `top` dépasse de acos(occluder / top) au-delà.
        const double horizon = std::acos(occluder / distance) + (top > occluder ? std::acos(occluder / top) : 0.0);
        const double cosine = Math::Dot(camera, center) / (distance * centerLength);
        const double angle = std::acos(std::clamp(cosine, -1.0, 1.0));
        const double angularRadius = std::asin(std::min(radius / centerLength, 1.0));
        return angle - angularRadius > horizon;
    }

    std::expected<std::unique_ptr<PlanetRenderer>, std::string> PlanetRenderer::Create(RHI::Device& device) {
        auto compute = PlanetTileCompute::Create(device);
        if (!compute) {
            return std::unexpected(compute.error());
        }
        return std::unique_ptr<PlanetRenderer>(new PlanetRenderer(device, std::move(*compute)));
    }

    void PlanetRenderer::BeginFrame(std::uint32_t frameIndex, std::chrono::steady_clock::time_point now) {
        ++m_frame;
        m_now = now;
        // Mesures des tuiles calculées avec cette image (le GPU l'a finie) : leurs hauteurs extrêmes donnent leur
        // sphère englobante (jupes comprises), à la place des estimations prudentes.
        const auto stats = m_compute->GetStats(frameIndex);
        const std::vector<Computed>& computed = m_computed[frameIndex];
        for (std::size_t i = 0; i < std::min(stats.size(), computed.size()); ++i) {
            if (PlanetState* state = FindOwner(computed[i].owner)) {
                if (const auto node = state->nodes.find(computed[i].key); node != state->nodes.end() && node->second.tile != NoTile) {
                    const TileKey key = TileKey::Unpack(computed[i].key);
                    node->second.lowest = stats[i].lowest;
                    node->second.highest = stats[i].highest;
                    // Un mètre de marge : les mesures sont en float, la sphère passe par quelques points seulement.
                    node->second.bounds = GetTileBounds(*state->terrain, key, node->second.lowest - 1.0, node->second.highest + 1.0);
                    node->second.tight = true;
                    node->second.measured = true;
                }
            }
        }
        m_computed[frameIndex].clear();
        if (!m_indices) {
            const std::vector<std::uint32_t> indices = BuildTileIndices();
            auto buffer = m_device.CreateBuffer({indices.size() * sizeof(std::uint32_t), RHI::BufferUsage::Index}, indices.data());
            if (!buffer) {
                Log::Error("Planète : {}", buffer.error());
                return;
            }
            m_indices = std::move(*buffer);
        }
    }

    PlanetRenderer::Node& PlanetRenderer::GetNode(PlanetState& state, TileKey key, const Node* parent) {
        auto [it, created] = state.nodes.try_emplace(key.Pack());
        Node& node = it->second;
        if (!created && (node.tight || !parent || !parent->measured)) {
            return node;
        }
        if (parent && parent->measured) {
            // Entre les hauteurs de la parente, plus ce que les octaves de l'enfant peuvent ajouter, plus une marge
            // pour le relief qui passe entre les sommets mesurés de la parente.
            const Terrain& terrain = *state.terrain;
            const double margin = terrain.GetOmittedRelief(terrain.GetOctaves(key.level - 1u)) + 0.1 * (parent->highest - parent->lowest) + 1.0;
            node.bounds = GetTileBounds(terrain, key, parent->lowest - margin, parent->highest + margin);
            node.tight = true;
        } else {
            node.bounds = GetTileBounds(*state.terrain, key);
        }
        return node;
    }

    bool PlanetRenderer::IsTileReady(Entity entity, TileKey key) const {
        const auto state = m_planets.find(entity);
        if (state == m_planets.end()) {
            return false;
        }
        const auto node = state->second.nodes.find(key.Pack());
        return node != state->second.nodes.end() && node->second.tile != NoTile;
    }

    void PlanetRenderer::Collect(Entity entity, const PlanetComponent& planet, const Math::Vector3d& position,
                                 const Math::Quaternionf& rotation, const ViewTest& inView, std::vector<Tile>& tiles,
                                 std::vector<Plain>* plains, std::uint32_t plainLevel, double plainRange) {
        if (!m_indices) {
            return;
        }
        PlanetState& state = m_planets[entity];
        if (planet.radius != state.radius || planet.relief != state.relief || planet.seed != state.seed || planet.mountainWidth != state.mountainWidth) {
            // Autre forme : toutes les tuiles sont à refaire.
            for (auto& [key, node] : state.nodes) {
                Retire(node);
            }
            state.nodes.clear();
            state.terrain = std::make_shared<const Terrain>(planet.radius, planet.relief, planet.seed, planet.mountainWidth);
            state.owner = m_nextOwner++; // les tuiles de l'ancienne forme encore en cours seront ignorées
            state.radius = planet.radius;
            state.relief = planet.relief;
            state.seed = planet.seed;
            state.mountainWidth = planet.mountainWidth;
        }
        state.lastUsed = m_frame;
        state.requests.clear();

        const Terrain& terrain = *state.terrain;
        const Math::Vector3d camera = rotation.Conjugate().Rotate(-position);
        const double maxRadius = terrain.GetRadius() + terrain.GetRelief();
        // Rien n'est plus bas que le sol le plus bas possible ; avec un océan, caméra au-dessus de lui, rien n'est plus
        // bas que sa surface non plus : ce qui passe sous son horizon (eau, fond, îles basses) est caché. Le fond vu
        // au travers de l'eau ne l'est qu'en rasant la surface, où elle reflète tout (fresnel de fragmentOcean).
        double minRadius = std::max(terrain.GetRadius() - terrain.GetRelief(), 1.0);
        if (const double sea = GetSeaRadius(planet); planet.ocean && sea > minRadius && Math::Length(camera) > sea) {
            minRadius = sea;
        }
        // Vitesse de la caméra, lissée sur un quart de seconde (une image lente ne la fait pas sauter).
        const double elapsed = std::chrono::duration<double>(m_now - state.time).count();
        if (state.time != std::chrono::steady_clock::time_point{} && elapsed > 0.0 && elapsed < 0.5) {
            const Math::Vector3d velocity = (camera - state.camera) / elapsed;
            state.velocity = Math::Length(velocity) > MaxCameraSpeed
                               ? Math::Vector3d{}
                               : state.velocity + (velocity - state.velocity) * std::min(elapsed / 0.25, 1.0);
        } else {
            state.velocity = {};
        }
        state.camera = camera;
        state.time = m_now;
        Visit visit{state, camera, position, rotation, camera + state.velocity * LookAhead, minRadius, maxRadius, inView, tiles,
                    plains, plainLevel, plainRange};
        // Les racines d'abord : la planète apparaît d'un coup, quand ses six racines sont calculées (la même image).
        bool rootsReady = true;
        for (std::uint8_t face = 0; face < 6; ++face) {
            Node& node = GetNode(state, {face, 0, 0, 0});
            node.lastUsed = m_frame;
            if (node.tile == NoTile) {
                rootsReady = false;
                state.requests.push_back({{face, 0, 0, 0}, 0.0});
            }
        }
        if (!rootsReady) {
            return;
        }
        for (std::uint8_t face = 0; face < 6; ++face) {
            const TileKey key{face, 0, 0, 0};
            VisitNode(visit, key, GetNode(state, key));
        }
    }

    bool PlanetRenderer::IsCulled(const Visit& visit, const TileBounds& bounds) const {
        // Derrière l'horizon du sol le plus bas, pour le plus haut point de cette tuile (et non de toute la planète) :
        // près du sol, une tuile basse au loin est cachée bien avant une montagne.
        const double top = std::min(Math::Length(bounds.center) + bounds.radius, visit.maxRadius);
        if (IsBehindHorizon(visit.camera, bounds.center, bounds.radius, visit.minRadius, top)) {
            return true;
        }
        const Math::Vector3d relative = visit.position + visit.rotation.Rotate(bounds.center);
        return !visit.inView(Math::Vector3f(relative), static_cast<float>(bounds.radius));
    }

    void PlanetRenderer::VisitNode(Visit& visit, TileKey key, Node& node) {
        node.lastUsed = m_frame;
        if (IsCulled(visit, node)) {
            return;
        }
        const double distance = Math::Length(visit.camera - node.bounds.center) - node.bounds.radius;
        const Terrain& terrain = *visit.state.terrain;
        if (visit.plains && key.level == visit.plainLevel && distance < visit.plainRange) {
            visit.plains->push_back({key, visit.position + visit.rotation.Rotate(node.bounds.origin), std::max(distance, 0.0)});
        }
        if (key.level < terrain.GetMaxLevel() && distance < GetSplitDistance(terrain, key.level, m_splitFactor)) {
            // Les enfants visibles et prêts sont dessinés ; à la place de ceux qui ne le sont pas encore (demandés), le
            // quart de cette tuile qu'ils couvrent. Toute la tuile à leur place : quand la caméra tournait, un enfant
            // qui entrait dans le champ remettait pour une image tout le sol grossier, jusque sous la caméra.
            std::array<Node*, 4> children{};
            std::array<bool, 4> culled{};
            std::uint32_t ready = 0;
            std::uint32_t visible = 0;
            for (std::uint32_t c = 0; c < 4; ++c) {
                const TileKey childKey = key.Child(c);
                children[c] = &GetNode(visit.state, childKey, &node);
                children[c]->lastUsed = m_frame;
                culled[c] = IsCulled(visit, *children[c]);
                if (culled[c]) {
                    continue;
                }
                ++visible;
                if (children[c]->tile != NoTile) {
                    ++ready;
                } else {
                    const double childDistance = Math::Length(visit.camera - children[c]->bounds.center) - children[c]->bounds.radius;
                    visit.state.requests.push_back({childKey, std::max(childDistance, 0.0)});
                }
            }
            if (ready > 0) {
                for (std::uint32_t c = 0; c < 4; ++c) {
                    if (culled[c]) {
                        continue;
                    }
                    if (children[c]->tile != NoTile) {
                        VisitNode(visit, key.Child(c), *children[c]);
                    } else if (node.tile != NoTile) {
                        AddTile(visit, key, node, children[c]->bounds, c);
                    }
                }
                return;
            }
            if (visible == 0) {
                return;
            }
        } else {
            Prefetch(visit, key, node);
        }
        if (node.tile != NoTile) {
            AddTile(visit, key, node, node.bounds, 4);
        }
    }

    // Tuile dessinée entière : ses descendants que demande le trajet de la caméra (LookAhead) sont calculés en avance,
    // sans être dessinés.
    void PlanetRenderer::Prefetch(Visit& visit, TileKey key, Node& node) {
        const Terrain& terrain = *visit.state.terrain;
        if (key.level >= terrain.GetMaxLevel()
            || DistanceToPath(visit.camera, visit.ahead, node.bounds.center) - node.bounds.radius
                   >= GetSplitDistance(terrain, key.level, m_splitFactor)) {
            return;
        }
        for (std::uint32_t c = 0; c < 4; ++c) {
            const TileKey childKey = key.Child(c);
            Node& child = GetNode(visit.state, childKey, &node);
            child.lastUsed = m_frame;
            if (IsCulled(visit, child)) {
                continue;
            }
            if (child.tile == NoTile) {
                const double childDistance = DistanceToPath(visit.camera, visit.ahead, child.bounds.center) - child.bounds.radius;
                visit.state.requests.push_back({childKey, std::max(childDistance, 0.0), true});
            } else {
                Prefetch(visit, childKey, child);
            }
        }
    }

    // La tuile `key` à dessiner, entière (quarter 4) ou le quart de l'enfant `quarter` (sa sphère : `bounds`), et les
    // plaines qu'elle couvre plus finement qu'elle.
    void PlanetRenderer::AddTile(Visit& visit, TileKey key, const Node& node, const TileBounds& bounds, std::uint32_t quarter) {
        const Terrain& terrain = *visit.state.terrain;
        const bool whole = quarter >= 4;
        const TileKey covered = whole ? key : key.Child(quarter);
        const double distance = Math::Length(visit.camera - bounds.center) - bounds.radius;
        // Au plus 3 niveaux plus grossière (64 plaines) : plus haut, c'est une tuile dont les enfants ne sont pas
        // encore prêts, pas le sol au loin.
        if (visit.plains && key.level < visit.plainLevel && key.level + 3u >= visit.plainLevel && distance < visit.plainRange) {
            if (whole) {
                AddPlains(visit, key);
            } else if (covered.level == visit.plainLevel) {
                visit.plains->push_back({covered, visit.position + visit.rotation.Rotate(bounds.origin), std::max(distance, 0.0)});
            } else {
                AddPlains(visit, covered);
            }
        }
        // Glisse vers la forme parente jusqu'à la distance où la parente reprend la place (une voisine plus grossière
        // n'est jamais plus proche) : là, son bord est celui de la parente, sans fente.
        float morphStart = 1.0e30f;
        float morphScale = 0.0f;
        if (key.level > 0) {
            const double end = GetSplitDistance(terrain, key.level - 1u, m_splitFactor);
            morphStart = static_cast<float>(MorphStart * end);
            morphScale = static_cast<float>(1.0 / ((1.0 - MorphStart) * end));
        }
        visit.tiles.push_back({node.tile, visit.position + visit.rotation.Rotate(node.bounds.origin), node.bounds.origin,
                               visit.position + visit.rotation.Rotate(bounds.center), static_cast<float>(bounds.radius),
                               morphStart, morphScale, node.lowest, whole ? 0u : quarter * TileQuarterIndices,
                               whole ? 4u * TileQuarterIndices : TileQuarterIndices, covered});
    }

    // Plaines sous une tuile dessinée plus grossière qu'elles (loin, ou vue de haut) : les arbres ne s'arrêtent pas
    // là où le sol devient moins fin. Leurs bornes sont celles de tout le relief (pas de nœud, donc pas de mesure) :
    // un peu plus de plaines retenues que nécessaire, que le tri des arbres écarte.
    void PlanetRenderer::AddPlains(Visit& visit, TileKey key) {
        const Terrain& terrain = *visit.state.terrain;
        for (std::uint32_t c = 0; c < 4; ++c) {
            const TileKey child = key.Child(c);
            const TileBounds bounds = GetTileBounds(terrain, child);
            if (IsCulled(visit, bounds)) {
                continue;
            }
            const double distance = Math::Length(visit.camera - bounds.center) - bounds.radius;
            if (distance >= visit.plainRange) {
                continue;
            }
            if (child.level == visit.plainLevel) {
                visit.plains->push_back({child, visit.position + visit.rotation.Rotate(bounds.origin), std::max(distance, 0.0)});
            } else {
                AddPlains(visit, child);
            }
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

    // Tuile confiée au GPU, calculée au début de l'image suivante (PlanetRenderer::Compute) : sa case est prête pour
    // tout dessin qui suit. false si l'image est pleine.
    bool PlanetRenderer::Schedule(PlanetState& state, TileKey key, Node& node, std::uint32_t frameIndex) {
        // Une case libérée que le GPU ne lit plus, sinon une nouvelle.
        std::uint32_t tile = NoTile;
        const auto free = std::ranges::find_if(m_freeTiles, [this](const FreeTile& f) { return f.frame + RHI::FramesInFlight < m_frame; });
        if (free != m_freeTiles.end()) {
            tile = free->tile;
            m_freeTiles.erase(free);
        } else {
            auto created = m_compute->CreateTile();
            if (!created) {
                if (!std::exchange(m_poolFullLogged, true))
                    Log::Error("Planète : {}", created.error());
                return false;
            }
            tile = *created;
        }
        if (!m_compute->Add(frameIndex, *state.terrain, key, tile)) {
            m_freeTiles.push_back({tile, 0});
            return false;
        }
        m_computed[frameIndex].push_back({state.owner, key.Pack()});
        node.tile = tile;
        // Point le plus bas prudent jusqu'au retour des mesures du GPU.
        node.lowest = state.terrain->GetRadius() - 1.5 * state.terrain->GetRelief() - node.bounds.edge;
        node.measured = false;
        node.lastUsed = m_frame;
        return true;
    }

    void PlanetRenderer::Retire(Node& node) {
        if (node.tile != NoTile) {
            m_freeTiles.push_back({node.tile, node.lastUsed});
            node.tile = NoTile;
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
        // Celles demandées en avance ne retiennent pas l'écran de chargement. Elles ne passent pas après celles à
        // dessiner : en vol rapide au ras du sol, les tuiles fines à dessiner prennent presque tout le budget de
        // l'image, et les grossières du trajet n'arriveraient jamais ; une tuile fine qui manque laisse voir le quart
        // de sa parente, sans trou.
        m_pendingTiles = static_cast<std::uint32_t>(std::ranges::count_if(pending, [](const Pending& p) { return !p.request.ahead; }));
        std::ranges::sort(pending, [](const Pending& a, const Pending& b) {
            return a.request.key.level != b.request.key.level ? a.request.key.level < b.request.key.level
                                                              : a.request.distance < b.request.distance;
        });
        for (const Pending& item : pending) {
            Node& node = GetNode(*item.state, item.request.key);
            if (node.tile == NoTile && !Schedule(*item.state, item.request.key, node, frameIndex)) {
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
        const double normalFactor = GetSplitFactor(GroundDetail::Low);
        const auto maxNodes = static_cast<std::size_t>(static_cast<double>(MaxNodes) * (m_splitFactor * m_splitFactor) / (normalFactor * normalFactor));
        for (auto& [entity, state] : m_planets) {
            if (state.nodes.size() <= maxNodes) {
                continue;
            }
            std::vector<std::pair<std::uint64_t, std::uint64_t>> old; // dernière visite, clé
            for (const auto& [key, node] : state.nodes) {
                if (node.lastUsed != m_frame && (key >> 59) != 0) {
                    old.emplace_back(node.lastUsed, key);
                }
            }
            const std::size_t excess = std::min(state.nodes.size() - maxNodes, old.size());
            std::ranges::nth_element(old, old.begin() + static_cast<std::ptrdiff_t>(excess));
            for (std::size_t i = 0; i < excess; ++i) {
                auto node = state.nodes.find(old[i].second);
                Retire(node->second);
                state.nodes.erase(node);
            }
        }
    }
}
