#include "Planetology/PlanetGround.hpp"

#include "Flux/Scene/Scene.hpp"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <numbers>
#include <stop_token>
#include <thread>
#include <unordered_set>

namespace Flux::Planetology {
    namespace {
        // Tuile plus demandée depuis tant de mises à jour (deux secondes à 60 images/s) : retirée.
        constexpr std::uint64_t KeepUpdates = 120;
        // La gravité d'une planète s'arrête à tant de fois son rayon (de son centre). Entre GravityBlend et
        // GravityReach rayons, elle se fond dans celle du monde : pas de bascule d'un coup (un sol plat de la scène
        // posé à la limite garderait sinon un bas penché vers la planète).
        constexpr double GravityReach = 2.0;
        constexpr double GravityBlend = 1.5;
        // Rayon au plus du disque de tuiles autour d'une sonde, en mètres.
        constexpr double MaxProbeDisc = 200.0;

        JPH::RVec3 ToJolt(const Math::Vector3d& v) {
            return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
        }

        // Maillage Jolt d'une tuile, sommets relatifs à `center` (repère de la planète). nullptr si Jolt le refuse.
        JPH::RefConst<JPH::Shape> BuildShape(const Terrain& terrain, TileKey key, Math::Vector3d& center) {
            thread_local std::vector<Math::Vector3f> positions;
            BuildTileSurface(terrain, key, center, positions);
            JPH::VertexList vertices;
            vertices.reserve(positions.size());
            for (const Math::Vector3f& p : positions) {
                vertices.push_back(JPH::Float3(p.x, p.y, p.z));
            }
            // Comme BuildTileIndices (sans les jupes) : sens trigonométrique vu de dehors, la face que Jolt fait toucher.
            JPH::IndexedTriangleList triangles;
            triangles.reserve(2 * TileQuads * TileQuads);
            for (std::uint32_t j = 0; j < TileQuads; ++j) {
                for (std::uint32_t i = 0; i < TileQuads; ++i) {
                    const std::uint32_t a = j * TileSide + i;
                    const std::uint32_t b = a + 1;
                    const std::uint32_t c = a + TileSide + 1;
                    const std::uint32_t d = a + TileSide;
                    triangles.push_back(JPH::IndexedTriangle(a, b, c));
                    triangles.push_back(JPH::IndexedTriangle(a, c, d));
                }
            }
            const JPH::ShapeSettings::ShapeResult result = JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
            return result.IsValid() ? result.Get() : nullptr;
        }
    }

    // Fil qui fait les tuiles : file remplacée à chaque mise à jour par les tuiles manquantes, les plus proches
    // d'abord ; la tuile en cours finit toujours.
    struct PlanetGround::Worker {
        struct Job
        {
            std::shared_ptr<const Terrain> terrain;
            std::uint64_t owner = 0;
            TileKey key;
        };
        struct Done
        {
            std::uint64_t owner = 0;
            TileKey key;
            Math::Vector3d center;
            JPH::RefConst<JPH::Shape> shape;
        };

        std::mutex mutex;
        std::condition_variable_any wake;
        std::deque<Job> jobs;
        std::vector<Done> done;
        std::jthread thread{[this](std::stop_token stop) { Run(stop); }}; // en dernier : arrêté avant le reste

        void Run(std::stop_token stop) {
            while (true) {
                Job job;
                {
                    std::unique_lock lock(mutex);
                    if (!wake.wait(lock, stop, [&] { return !jobs.empty(); })) {
                        return;
                    }
                    job = std::move(jobs.front());
                    jobs.pop_front();
                }
                Done result{job.owner, job.key, {}, nullptr};
                result.shape = BuildShape(*job.terrain, job.key, result.center);
                std::scoped_lock lock(mutex);
                done.push_back(std::move(result));
            }
        }

        void Replace(std::vector<Job>& next) {
            {
                std::scoped_lock lock(mutex);
                jobs.assign(std::make_move_iterator(next.begin()), std::make_move_iterator(next.end()));
            }
            next.clear();
            wake.notify_one();
        }

        void Collect(std::vector<Done>& results) {
            std::scoped_lock lock(mutex);
            std::swap(results, done);
        }
    };

    PlanetGround::PlanetGround(JPH::ObjectLayer layer) : m_layer(layer), m_worker(std::make_unique<Worker>()) {}

    PlanetGround::~PlanetGround() = default;

    void PlanetGround::RemoveTiles(JPH::PhysicsSystem& system, PlanetState& state) {
        JPH::BodyInterface& bodies = system.GetBodyInterface();
        for (const auto& [packed, tile] : state.tiles) {
            if (!tile.body.IsInvalid()) {
                bodies.RemoveBody(tile.body);
                bodies.DestroyBody(tile.body);
            }
        }
        state.tiles.clear();
        state.owner = m_nextOwner++; // les tuiles encore en cours seront ignorées
    }

    void PlanetGround::AddTile(JPH::PhysicsSystem& system, Entity entity, PlanetState& state, TileKey key,
                               const Math::Vector3d& center, const JPH::Shape& shape) {
        const auto found = state.tiles.find(key.Pack());
        if (found == state.tiles.end() || !found->second.body.IsInvalid()) {
            return; // plus demandée, ou déjà faite (tout de suite, pendant qu'elle était en cours)
        }
        const Math::Quaternionf& r = state.rotation;
        JPH::BodyCreationSettings settings(&shape, ToJolt(state.position + r.Rotate(center)), JPH::Quat(r.x, r.y, r.z, r.w).Normalized(),
                                           JPH::EMotionType::Static, m_layer);
        settings.mUserData = static_cast<JPH::uint64>(entt::to_integral(entity));
        settings.mFriction = 0.8f; // terre, rochers
        found->second.body = system.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
    }

    void PlanetGround::Request(const PlanetState& state, const Probe& probe, std::vector<TileKey>& keys, bool& underMissing,
                               TileKey& under) const {
        underMissing = false;
        const Terrain& terrain = *state.terrain;
        const Math::Vector3d local = state.rotation.Conjugate().Rotate(probe.position - state.position);
        const double distance = Math::Length(local);
        if (distance < 1.0e-3) {
            return;
        }
        const Math::Vector3d direction = local / distance;
        const std::uint32_t level = terrain.GetMaxLevel();
        const double radius = terrain.GetRadius();
        const double altitude = distance - (radius + terrain.GetHeight(direction, terrain.GetOctaves(level)));
        if (altitude > probe.reach) {
            return; // trop haut : rien à toucher avant longtemps
        }
        under = FindTile(direction, level);
        const auto found = state.tiles.find(under.Pack());
        // Tout près du sol et sans rien dessous : la tuile ne peut pas attendre le fil (il passerait au travers).
        underMissing = (found == state.tiles.end() || found->second.body.IsInvalid()) && altitude < 3.0 + 0.25 * probe.speed;
        keys.push_back(under);

        // Les tuiles autour, dans le rayon `reach` : des points du plan tangent, plus serrés que les tuiles (dont le
        // bord fait de 0,8 à 1,2 fois sa valeur au centre d'une face), ramenés sur la sphère.
        const double edge = 0.5 * std::numbers::pi * radius / static_cast<double>(std::uint64_t{1} << level);
        const double spacing = 0.4 * edge;
        // Le disque est borné : sa hauteur suit la vitesse (une seconde de chute), pas sa largeur. À 15 km/s, un disque
        // de 15 km ferait des centaines de milliers de tuiles par image ; ce qui est devant un objet rapide vient de
        // sondes posées sur son chemin (PhysicsWorld::CollectProbes).
        const double reach = std::max(std::min(probe.reach, MaxProbeDisc), edge * 0.5) + spacing;
        const Math::Vector3d side = std::abs(direction.y) < 0.9 ? Math::Vector3d{0.0, 1.0, 0.0} : Math::Vector3d{1.0, 0.0, 0.0};
        const Math::Vector3d e1 = Math::Normalize(Math::Cross(direction, side));
        const Math::Vector3d e2 = Math::Cross(direction, e1);
        const int steps = static_cast<int>(std::ceil(reach / spacing));
        // Des plus proches aux plus lointaines : la file du fil les fait dans cet ordre.
        thread_local std::vector<std::pair<int, TileKey>> samples;
        samples.clear();
        for (int b = -steps; b <= steps; ++b) {
            for (int a = -steps; a <= steps; ++a) {
                const int square = a * a + b * b;
                if (square * spacing * spacing > reach * reach) {
                    continue;
                }
                const Math::Vector3d point = direction * radius + e1 * (a * spacing) + e2 * (b * spacing);
                samples.emplace_back(square, FindTile(Math::Normalize(point), level));
            }
        }
        std::ranges::sort(samples, {}, &std::pair<int, TileKey>::first);
        for (const auto& [square, key] : samples) {
            keys.push_back(key);
        }
    }

    void PlanetGround::Update(const Scene& scene, const Math::WorldPosition& anchor, std::span<const Probe> probes,
                              JPH::PhysicsSystem& system, bool wait) {
        ++m_update;
        for (auto& [entity, state] : m_planets) {
            state.seen = false;
        }
        for (const auto [entity, planet] : scene.GetRegistry().view<const PlanetComponent>().each()) {
            if (scene.GetShip(entity) != NullEntity) {
                continue; // une planète dans un vaisseau : un décor, pas un sol
            }
            PlanetState& state = m_planets[entity];
            if (!state.terrain || planet.radius != state.radius || planet.relief != state.relief || planet.seed != state.seed) {
                RemoveTiles(system, state);
                state.terrain = std::make_shared<const Terrain>(planet.radius, planet.relief, planet.seed);
                state.radius = planet.radius;
                state.relief = planet.relief;
                state.seed = planet.seed;
            }
            const Math::Vector3d position = Math::Delta(anchor, scene.GetWorldPosition(entity));
            const Math::Quaternionf rotation = scene.GetWorldRotation(entity);
            if (!state.tiles.empty() && (Math::Length(position - state.position) > 1.0e-3 || !(rotation == state.rotation))) {
                RemoveTiles(system, state); // la planète a bougé : son sol est à refaire
            }
            state.position = position;
            state.rotation = rotation;
            state.gravity = planet.gravity;
            state.atmosphere = planet.atmosphere;
            state.atmosphereHeight = planet.atmosphereHeight;
            state.seen = true;
        }
        std::erase_if(m_planets, [&](auto& entry) {
            if (entry.second.seen) {
                return false;
            }
            RemoveTiles(system, entry.second);
            return true;
        });
        if (m_planets.empty()) {
            return;
        }

        // Tuiles finies sur le fil.
        thread_local std::vector<Worker::Done> results;
        m_worker->Collect(results);
        for (const Worker::Done& done : results) {
            for (auto& [entity, state] : m_planets) {
                if (state.owner == done.owner && done.shape) {
                    AddTile(system, entity, state, done.key, done.center, *done.shape);
                }
            }
        }
        results.clear();

        // Tuiles voulues autour de chaque sonde ; celle sous une sonde au ras du sol, tout de suite.
        thread_local std::vector<TileKey> keys;
        thread_local std::vector<Worker::Job> jobs;
        jobs.clear();
        thread_local std::unordered_set<std::uint64_t> queued;
        for (auto& [entity, state] : m_planets) {
            queued.clear();
            for (const Probe& probe : probes) {
                keys.clear();
                bool underMissing = false;
                TileKey under;
                Request(state, probe, keys, underMissing, under);
                for (const TileKey& key : keys) {
                    Tile& tile = state.tiles[key.Pack()];
                    tile.lastUsed = m_update;
                    const bool now = wait || (underMissing && key == under);
                    if (tile.body.IsInvalid() && now) {
                        Math::Vector3d center;
                        if (const JPH::RefConst<JPH::Shape> shape = BuildShape(*state.terrain, key, center)) {
                            AddTile(system, entity, state, key, center, *shape);
                        }
                    }
                    if (tile.body.IsInvalid() && queued.insert(key.Pack()).second) {
                        jobs.push_back({state.terrain, state.owner, key});
                    }
                }
            }
        }
        m_worker->Replace(jobs);

        // Tuiles plus demandées depuis un moment.
        JPH::BodyInterface& bodies = system.GetBodyInterface();
        for (auto& [entity, state] : m_planets) {
            std::erase_if(state.tiles, [&](const auto& entry) {
                const Tile& tile = entry.second;
                if (tile.lastUsed + KeepUpdates >= m_update) {
                    return false;
                }
                if (!tile.body.IsInvalid()) {
                    bodies.RemoveBody(tile.body);
                    bodies.DestroyBody(tile.body);
                }
                return true;
            });
        }
    }

    void PlanetGround::Shift(JPH::PhysicsSystem& system, const Math::Vector3d& shift) {
        JPH::BodyInterface& bodies = system.GetBodyInterface();
        const JPH::Vec3 offset(static_cast<float>(shift.x), static_cast<float>(shift.y), static_cast<float>(shift.z));
        for (auto& [entity, state] : m_planets) {
            state.position += shift;
            for (const auto& [packed, tile] : state.tiles) {
                if (!tile.body.IsInvalid()) {
                    bodies.SetPosition(tile.body, bodies.GetPosition(tile.body) + offset, JPH::EActivation::DontActivate);
                }
            }
        }
    }

    void PlanetGround::Clear(JPH::PhysicsSystem& system) {
        for (auto& [entity, state] : m_planets) {
            RemoveTiles(system, state);
        }
        m_planets.clear();
    }

    Math::Vector3f PlanetGround::GetGravity(const Math::Vector3d& position, const Math::Vector3f& fallback) const {
        // La planète qui attire le plus ; sa part (1 jusqu'à GravityBlend rayons, 0 à GravityReach) remplace
        // d'autant la gravité du monde.
        double strongest = 0.0;
        double weight = 0.0;
        Math::Vector3d pull;
        for (const auto& [entity, state] : m_planets) {
            const Math::Vector3d offset = position - state.position;
            const double distance = Math::Length(offset);
            const double radius = state.radius;
            if (distance >= GravityReach * radius || distance < 1.0e-3) {
                continue;
            }
            const double t = std::clamp((GravityReach - distance / radius) / (GravityReach - GravityBlend), 0.0, 1.0);
            const double part = t * t * (3.0 - 2.0 * t);
            const double ratio = std::min(1.0, radius / distance);
            const double strength = state.gravity * ratio * ratio;
            if (part * strength > strongest || (strongest == 0.0 && part > weight)) {
                strongest = part * strength;
                weight = part;
                pull = offset * (-strength / distance);
            }
        }
        if (weight <= 0.0) {
            return fallback;
        }
        return Math::Vector3f(pull * weight) + fallback * static_cast<float>(1.0 - weight);
    }

    float PlanetGround::GetAir(const Math::Vector3d& position, float fallback) const {
        double weight = 0.0;
        double air = 0.0;
        for (const auto& [entity, state] : m_planets) {
            const double distance = Math::Length(position - state.position);
            const double radius = state.radius;
            if (distance >= GravityReach * radius) {
                continue;
            }
            const double t = std::clamp((GravityReach - distance / radius) / (GravityReach - GravityBlend), 0.0, 1.0);
            weight = std::max(weight, t * t * (3.0 - 2.0 * t));
            // Comme le ciel (Atmosphere.cpp) : densité divisée par e tous les huitièmes de sa hauteur, nulle au-dessus.
            const double height = std::max(static_cast<double>(state.atmosphereHeight), 100.0);
            const double above = std::max(distance - radius, 0.0);
            if (state.atmosphere > 0.0f && above < height) {
                air = std::max(air, state.atmosphere * std::exp(-8.0 * above / height));
            }
        }
        return static_cast<float>(air + fallback * (1.0 - weight));
    }

    int PlanetGround::GetTileCount() const {
        int count = 0;
        for (const auto& [entity, state] : m_planets) {
            count += static_cast<int>(std::ranges::count_if(state.tiles, [](const auto& entry) { return !entry.second.body.IsInvalid(); }));
        }
        return count;
    }
}
