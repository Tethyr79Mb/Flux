#include "Flux/Graphics/Planet.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>
#include <cmath>
#include <numbers>
#include <random>

namespace Flux::Planet {
    namespace {
        // Repère de chaque face : sa normale et ses axes s et t, avec s x t = normale (sommets dans le sens
        // trigonométrique vu de dehors, comme le cube du moteur).
        struct Face
        {
            Math::Vector3d normal;
            Math::Vector3d s;
            Math::Vector3d t;
        };
        constexpr std::array<Face, 6> Faces{{
            {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
            {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
            {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
            {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
            {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
            {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
        }};

        constexpr double QuarterPi = 0.25 * std::numbers::pi;

        // Fréquence du bruit sur la sphère unité : 1,5 donne quelques continents.
        constexpr double BaseFrequency = 1.5;
        // Octaves au plus (la plus fine fait quelques mètres sur une planète comme la Terre).
        constexpr std::uint32_t MaxOctaves = 22;
        // Le relief des montagnes (bruit en crêtes) s'arrête plus tôt : il coûte autant que le reste.
        constexpr std::uint32_t MaxRidgeOctaves = 8;
        // Somme des amplitudes de toutes les octaves (1 + 1/2 + 1/4...) : le relief garde la même échelle quel que
        // soit le nombre d'octaves d'une tuile.
        constexpr double AmplitudeSum = 2.0;
        // Relief de détail (fractions du relief) : à partir de l'octave 6 (environ 4 km de longueur d'onde sur une
        // planète de 60 km), 5 % du relief, puis x 0,7 par octave (contre 0,5 pour les continents) : pour 800 m de
        // relief, environ 6 m de bosses à 100 m d'écart et 1,5 m à 15 m.
        constexpr std::uint32_t DetailFirstOctave = 6;
        constexpr double DetailAmplitude = 0.05;
        constexpr double DetailGain = 0.7;

        double Fade(double t) {
            return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
        }

        double Gradient(std::uint8_t hash, double x, double y, double z) {
            const std::uint8_t h = hash & 15u;
            const double u = h < 8 ? x : y;
            const double v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
            return ((h & 1u) ? -u : u) + ((h & 2u) ? -v : v);
        }

        double Lerp(double a, double b, double t) {
            return a + (b - a) * t;
        }

        // Coordonnée de la grille de toute la face (de 0 à TileQuads x 2^niveau) en s ou t de -1 à 1 : calculée depuis
        // un entier, donc identique pour un sommet partagé par deux tuiles voisines.
        double GridToFace(std::int64_t index, std::uint32_t level) {
            const double cells = static_cast<double>(TileQuads) * static_cast<double>(std::uint64_t{1} << level);
            return -1.0 + 2.0 * static_cast<double>(index) / cells;
        }
    }

    Math::Vector3d DirectionFromFace(std::uint32_t face, double s, double t) {
        const Face& f = Faces[face % 6];
        const double a = std::tan(s * QuarterPi);
        const double b = std::tan(t * QuarterPi);
        return Math::Normalize(f.normal + f.s * a + f.t * b);
    }

    void FaceFromDirection(const Math::Vector3d& direction, std::uint32_t& face, double& s, double& t) {
        const double ax = std::abs(direction.x);
        const double ay = std::abs(direction.y);
        const double az = std::abs(direction.z);
        if (ax >= ay && ax >= az) {
            face = direction.x >= 0.0 ? 0 : 1;
        } else if (ay >= az) {
            face = direction.y >= 0.0 ? 2 : 3;
        } else {
            face = direction.z >= 0.0 ? 4 : 5;
        }
        const Face& f = Faces[face];
        const double along = Math::Dot(direction, f.normal);
        if (along <= 0.0) {
            s = t = 0.0; // direction nulle
            return;
        }
        const Math::Vector3d onCube = direction / along;
        s = std::atan(Math::Dot(onCube, f.s)) / QuarterPi;
        t = std::atan(Math::Dot(onCube, f.t)) / QuarterPi;
    }

    Terrain::Terrain(double radius, double relief, std::int32_t seed)
        : m_radius(std::max(radius, 1.0)), m_relief(std::max(relief, 0.0)) {
        // Environ 1,5 m entre deux sommets au niveau le plus fin.
        const double faceEdge = 0.5 * std::numbers::pi * m_radius;
        const double levels = std::ceil(std::log2(faceEdge / (1.5 * static_cast<double>(TileQuads))));
        m_maxLevel = static_cast<std::uint32_t>(std::clamp(levels, 0.0, static_cast<double>(MaxLevel)));

        // Permutation tirée de la graine, mélangée à la main : std::shuffle ne donne pas le même ordre selon le
        // compilateur, std::mt19937 si.
        std::array<std::uint8_t, 256> values{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            values[i] = static_cast<std::uint8_t>(i);
        }
        std::mt19937 random(static_cast<std::uint32_t>(seed));
        for (std::uint32_t i = 255; i > 0; --i) {
            std::swap(values[i], values[random() % (i + 1)]);
        }
        for (std::uint32_t i = 0; i < 512; ++i) {
            m_permutation[i] = values[i & 255u];
        }
    }

    std::uint32_t Terrain::GetOctaves(std::uint32_t level) const {
        // L'octave k a une longueur d'onde de 2π/(BaseFrequency.2^k) rayons ; les sommets d'un niveau sont espacés
        // de (π/2)/(TileQuads.2^niveau) rayons. On garde les octaves au moins deux fois plus grandes que cet écart.
        const double spacing = 0.5 * std::numbers::pi / (static_cast<double>(TileQuads) * std::ldexp(1.0, static_cast<int>(level)));
        const double wavelength = 2.0 * std::numbers::pi / BaseFrequency;
        const double octaves = std::floor(std::log2(wavelength / (2.0 * spacing))) + 1.0;
        return static_cast<std::uint32_t>(std::clamp(octaves, 1.0, static_cast<double>(MaxOctaves)));
    }

    double Terrain::Noise(const Math::Vector3d& p) const {
        const double fx = std::floor(p.x);
        const double fy = std::floor(p.y);
        const double fz = std::floor(p.z);
        const auto X = static_cast<std::uint32_t>(static_cast<std::int64_t>(fx) & 255);
        const auto Y = static_cast<std::uint32_t>(static_cast<std::int64_t>(fy) & 255);
        const auto Z = static_cast<std::uint32_t>(static_cast<std::int64_t>(fz) & 255);
        const double x = p.x - fx;
        const double y = p.y - fy;
        const double z = p.z - fz;
        const double u = Fade(x);
        const double v = Fade(y);
        const double w = Fade(z);
        const auto& P = m_permutation;
        const std::uint32_t A = P[X] + Y;
        const std::uint32_t AA = P[A] + Z;
        const std::uint32_t AB = P[A + 1] + Z;
        const std::uint32_t B = P[X + 1] + Y;
        const std::uint32_t BA = P[B] + Z;
        const std::uint32_t BB = P[B + 1] + Z;
        return Lerp(Lerp(Lerp(Gradient(P[AA], x, y, z), Gradient(P[BA], x - 1, y, z), u),
                         Lerp(Gradient(P[AB], x, y - 1, z), Gradient(P[BB], x - 1, y - 1, z), u), v),
                    Lerp(Lerp(Gradient(P[AA + 1], x, y, z - 1), Gradient(P[BA + 1], x - 1, y, z - 1), u),
                         Lerp(Gradient(P[AB + 1], x, y - 1, z - 1), Gradient(P[BB + 1], x - 1, y - 1, z - 1), u), v),
                    w);
    }

    double Terrain::GetHeight(const Math::Vector3d& direction, std::uint32_t octaves) const {
        if (m_relief <= 0.0) {
            return 0.0;
        }
        // Continents : bruit fractal (fBm) des grandes formes, chaque octave deux fois plus fine et deux fois plus
        // basse. Seul, il donne un sol lisse de près (à 100 m d'écart, quelques dizaines de centimètres de relief).
        double base = 0.0;
        double amplitude = 1.0;
        Math::Vector3d p = direction * BaseFrequency;
        for (std::uint32_t k = 0; k < std::min(octaves, DetailFirstOctave); ++k) {
            base += amplitude * Noise(p);
            amplitude *= 0.5;
            p *= 2.0;
        }
        base /= 0.5 * AmplitudeSum; // environ -1 à 1
        // Montagnes : crêtes (1 - |bruit|)², seulement sur les terres hautes. Le décalage sépare les deux bruits
        // (sinon les crêtes suivraient les côtes).
        double ridge = 0.0;
        amplitude = 1.0;
        p = direction * (4.0 * BaseFrequency) + Math::Vector3d{17.3, -41.7, 8.9};
        for (std::uint32_t k = 0; k < std::min(octaves, MaxRidgeOctaves); ++k) {
            const double n = 1.0 - std::abs(Noise(p));
            ridge += amplitude * n * n;
            amplitude *= 0.5;
            p *= 2.0;
        }
        ridge /= AmplitudeSum; // 0 à 1
        const double land = std::clamp(base * 3.0, 0.0, 1.0);
        // Détail : collines, puis bosses et rochers. Les octaves fines baissent moins vite (DetailGain au lieu de
        // 0,5) : le sol reste accidenté à toutes les échelles, plus sur les montagnes que dans les plaines.
        double detail = 0.0;
        amplitude = DetailAmplitude;
        p = direction * (BaseFrequency * std::ldexp(1.0, static_cast<int>(DetailFirstOctave))) + Math::Vector3d{-23.1, 5.7, 31.3};
        for (std::uint32_t k = DetailFirstOctave; k < octaves; ++k) {
            detail += amplitude * Noise(p);
            amplitude *= DetailGain;
            p *= 2.0;
        }
        detail *= 0.5 + 1.5 * ridge * land;
        return m_relief * std::clamp(0.6 * base + 0.6 * ridge * land + detail, -1.0, 1.0);
    }

    double Terrain::GetOmittedRelief(std::uint32_t octaves) const {
        // Ce que les octaves de rang >= octaves peuvent encore ajouter : la fin des continents et des crêtes, et
        // celle du détail (somme géométrique), au plus fort (x 2 sur les montagnes).
        const auto from = static_cast<int>(std::min(octaves, MaxRidgeOctaves));
        double omitted = 1.2 * 2.0 * std::ldexp(1.0, -from);
        const std::uint32_t firstDetail = std::max(octaves, DetailFirstOctave);
        omitted += 2.0 * DetailAmplitude * std::pow(DetailGain, firstDetail - DetailFirstOctave) / (1.0 - DetailGain);
        return m_relief * omitted;
    }

    double GetSplitDistance(const Terrain& terrain, std::uint32_t level) {
        // Angle d'une tuile le long des axes de sa face : (π/2) / 2^niveau.
        return LodSplitFactor * 0.5 * std::numbers::pi * terrain.GetRadius() * std::ldexp(1.0, -static_cast<int>(level));
    }

    TileBounds GetTileBounds(const Terrain& terrain, TileKey key) {
        const double R = terrain.GetRadius();
        const double relief = terrain.GetRelief();
        const auto x0 = static_cast<std::int64_t>(key.x) * TileQuads;
        const auto y0 = static_cast<std::int64_t>(key.y) * TileQuads;
        const auto at = [&](std::int64_t i, std::int64_t j) {
            return DirectionFromFace(key.face, GridToFace(x0 + i, key.level), GridToFace(y0 + j, key.level));
        };
        constexpr auto half = static_cast<std::int64_t>(TileQuads / 2);
        constexpr auto full = static_cast<std::int64_t>(TileQuads);
        TileBounds bounds;
        bounds.center = at(half, half) * R;
        // Les coins et les milieux des bords, au plus bas et au plus haut du relief (et sous la jupe).
        for (const std::int64_t j : {std::int64_t{0}, half, full}) {
            for (const std::int64_t i : {std::int64_t{0}, half, full}) {
                const Math::Vector3d d = at(i, j);
                for (const double r : {R - 1.5 * relief - 1.0, R + relief}) {
                    bounds.radius = std::max(bounds.radius, Math::Length(d * r - bounds.center));
                }
            }
        }
        bounds.edge = Math::Length(at(full, 0) - at(0, 0)) * R;
        return bounds;
    }

    void BuildTile(const Terrain& terrain, TileKey key, TileMesh& mesh) {
        const double R = terrain.GetRadius();
        const std::uint32_t octaves = terrain.GetOctaves(key.level);
        // Grille avec une rangée de plus tout autour : les normales du bord se calculent comme celles du milieu, et
        // celles des deux tuiles voisines tombent pareil (pas de marche de lumière).
        constexpr std::uint32_t Wide = TileSide + 2;
        thread_local std::vector<Math::Vector3d> positions;
        thread_local std::vector<Math::Vector3d> directions;
        positions.resize(Wide * Wide);
        directions.resize(Wide * Wide);
        const auto x0 = static_cast<std::int64_t>(key.x) * TileQuads - 1;
        const auto y0 = static_cast<std::int64_t>(key.y) * TileQuads - 1;
        for (std::uint32_t j = 0; j < Wide; ++j) {
            const double t = GridToFace(y0 + j, key.level);
            for (std::uint32_t i = 0; i < Wide; ++i) {
                const Math::Vector3d d = DirectionFromFace(key.face, GridToFace(x0 + i, key.level), t);
                directions[j * Wide + i] = d;
                positions[j * Wide + i] = d * (R + terrain.GetHeight(d, octaves));
            }
        }

        constexpr auto half = static_cast<std::int64_t>(TileQuads / 2);
        mesh.center = DirectionFromFace(key.face, GridToFace(x0 + 1 + half, key.level), GridToFace(y0 + 1 + half, key.level)) * R;
        mesh.vertices.resize(TileVertexCount);
        mesh.radius = 0.0;
        const auto vertex = [&](std::uint32_t i, std::uint32_t j) -> ModelVertex& { return mesh.vertices[j * TileSide + i]; };
        for (std::uint32_t j = 0; j < TileSide; ++j) {
            for (std::uint32_t i = 0; i < TileSide; ++i) {
                const std::uint32_t w = (j + 1) * Wide + (i + 1);
                const Math::Vector3d along = positions[w + 1] - positions[w - 1];
                const Math::Vector3d across = positions[w + Wide] - positions[w - Wide];
                const Math::Vector3d normal = Math::Normalize(Math::Cross(along, across));
                const Math::Vector3d local = positions[w] - mesh.center;
                vertex(i, j) = {Math::Vector3f(local), Math::Vector3f(normal), {0.0f, 0.0f}};
                mesh.radius = std::max(mesh.radius, Math::Length(local));
            }
        }

        // Forme de la parente : ses sommets (un sur deux de la tuile dans chaque sens) avec son nombre d'octaves, et ses
        // normales, calculées comme elle le fait (voisins à deux pas, une rangée de plus autour). Un sommet entre deux
        // des siens va au milieu du bord de son triangle (la diagonale pour le centre d'un carré : sommets a et c dans
        // BuildTileIndices).
        mesh.morph.resize(TileVertexCount);
        if (key.level == 0) {
            for (std::uint32_t v = 0; v < TileSide * TileSide; ++v) {
                mesh.morph[v] = {{0.0f, 0.0f, 0.0f}, mesh.vertices[v].normal};
            }
        } else {
            constexpr std::uint32_t Coarse = TileQuads / 2 + 3; // de -2 à TileQuads + 2, de deux en deux
            thread_local std::vector<Math::Vector3d> coarse;
            coarse.resize(Coarse * Coarse);
            const std::uint32_t parentOctaves = terrain.GetOctaves(key.level - 1u);
            for (std::uint32_t cj = 0; cj < Coarse; ++cj) {
                const double t = GridToFace(y0 + 1 + 2 * static_cast<std::int64_t>(cj) - 2, key.level);
                for (std::uint32_t ci = 0; ci < Coarse; ++ci) {
                    const Math::Vector3d d = DirectionFromFace(key.face, GridToFace(x0 + 1 + 2 * static_cast<std::int64_t>(ci) - 2, key.level), t);
                    coarse[cj * Coarse + ci] = d * (R + terrain.GetHeight(d, parentOctaves));
                }
            }
            // Sommet de la parente au point (i, j) de la tuile (i et j pairs, de 0 à TileQuads) et sa normale.
            const auto parent = [&](std::uint32_t i, std::uint32_t j) -> const Math::Vector3d& {
                return coarse[(j / 2 + 1) * Coarse + (i / 2 + 1)];
            };
            const auto parentNormal = [&](std::uint32_t i, std::uint32_t j) {
                const std::uint32_t c = (j / 2 + 1) * Coarse + (i / 2 + 1);
                return Math::Normalize(Math::Cross(coarse[c + 1] - coarse[c - 1], coarse[c + Coarse] - coarse[c - Coarse]));
            };
            for (std::uint32_t j = 0; j < TileSide; ++j) {
                for (std::uint32_t i = 0; i < TileSide; ++i) {
                    // Les deux sommets de la parente dont celui-ci est le milieu (le même deux fois s'il est à elle).
                    const std::uint32_t i0 = i & ~1u;
                    const std::uint32_t j0 = j & ~1u;
                    const std::uint32_t i1 = i0 + (i & 1u) * 2;
                    const std::uint32_t j1 = j0 + (j & 1u) * 2;
                    const Math::Vector3d target = (parent(i0, j0) + parent(i1, j1)) * 0.5;
                    const Math::Vector3d normal = Math::Normalize(parentNormal(i0, j0) + parentNormal(i1, j1));
                    const Math::Vector3d position = positions[(j + 1) * Wide + (i + 1)];
                    mesh.morph[j * TileSide + i] = {Math::Vector3f(target - position), Math::Vector3f(normal)};
                    mesh.radius = std::max(mesh.radius, Math::Length(target - mesh.center));
                }
            }
        }

        // Jupes : le bord parcouru dans le sens trigonométrique (bas, droite, haut, gauche), chaque sommet descendu
        // vers le centre de la planète. Assez profondes pour couvrir l'écart avec une voisine plus grossière (le
        // relief qu'elle n'a pas) ; la normale reste celle du bord, la jupe se confond avec le terrain.
        const double edge = Math::Length(directions[Wide + 1 + TileQuads] - directions[Wide + 1]) * R;
        const double depth = 0.05 * edge + 2.0 * terrain.GetOmittedRelief(octaves) + 1.0;
        std::uint32_t next = TileSide * TileSide;
        const auto skirt = [&](std::uint32_t i, std::uint32_t j) {
            const std::uint32_t w = (j + 1) * Wide + (i + 1);
            const Math::Vector3d local = positions[w] - directions[w] * depth - mesh.center;
            mesh.morph[next] = mesh.morph[j * TileSide + i]; // glisse avec son bord
            mesh.vertices[next++] = {Math::Vector3f(local), vertex(i, j).normal, {0.0f, 0.0f}};
            mesh.radius = std::max(mesh.radius, Math::Length(local + Math::Vector3d(mesh.morph[j * TileSide + i].offset)));
        };
        for (std::uint32_t k = 0; k < TileSide; ++k) {
            skirt(k, 0);
        }
        for (std::uint32_t k = 0; k < TileSide; ++k) {
            skirt(TileQuads, k);
        }
        for (std::uint32_t k = 0; k < TileSide; ++k) {
            skirt(TileQuads - k, TileQuads);
        }
        for (std::uint32_t k = 0; k < TileSide; ++k) {
            skirt(0, TileQuads - k);
        }
    }

    std::vector<std::uint32_t> BuildTileIndices() {
        std::vector<std::uint32_t> indices;
        indices.reserve(6 * TileQuads * TileQuads + 4 * 6 * TileQuads);
        for (std::uint32_t j = 0; j < TileQuads; ++j) {
            for (std::uint32_t i = 0; i < TileQuads; ++i) {
                const std::uint32_t a = j * TileSide + i;
                const std::uint32_t b = a + 1;
                const std::uint32_t c = a + TileSide + 1;
                const std::uint32_t d = a + TileSide;
                indices.insert(indices.end(), {a, b, c, a, c, d});
            }
        }
        // Le bord de la grille, dans l'ordre des jupes (BuildTile).
        const auto border = [](std::uint32_t edge, std::uint32_t k) {
            switch (edge) {
                case 0: return k;                                               // bas, s croissant
                case 1: return k * TileSide + TileQuads;                        // droite, t croissant
                case 2: return TileQuads * TileSide + (TileQuads - k);          // haut, s décroissant
                default: return (TileQuads - k) * TileSide;                     // gauche, t décroissant
            }
        };
        for (std::uint32_t edge = 0; edge < 4; ++edge) {
            const std::uint32_t first = TileSide * TileSide + edge * TileSide;
            for (std::uint32_t k = 0; k < TileQuads; ++k) {
                const std::uint32_t top0 = border(edge, k);
                const std::uint32_t top1 = border(edge, k + 1);
                const std::uint32_t low0 = first + k;
                const std::uint32_t low1 = first + k + 1;
                // Tournée vers l'extérieur de la tuile : (bord, jupe, bord suivant) puis (bord suivant, jupe, jupe suivante).
                indices.insert(indices.end(), {top0, low0, top1, top1, low0, low1});
            }
        }
        return indices;
    }

    struct TileGenerator::Impl
    {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::deque<Job> queue;
        std::vector<Result> done;
        std::vector<std::jthread> threads; // en dernier : arrêtés et attendus avant le reste

        void Run(std::stop_token stop) {
            for (;;) {
                Job job;
                {
                    std::unique_lock lock(mutex);
                    if (!wake.wait(lock, stop, [this] { return !queue.empty(); })) {
                        return; // arrêt demandé
                    }
                    job = std::move(queue.front());
                    queue.pop_front();
                }
                Result result{job.owner, job.key, {}};
                BuildTile(*job.terrain, job.key, result.mesh);
                std::lock_guard lock(mutex);
                done.push_back(std::move(result));
            }
        }
    };

    TileGenerator::TileGenerator(std::uint32_t threads) : m_impl(std::make_unique<Impl>()) {
        if (threads == 0) {
            // Laisser de quoi faire au fil principal, au GPU (pilote) et à la physique.
            const auto available = static_cast<std::int32_t>(std::thread::hardware_concurrency());
            threads = static_cast<std::uint32_t>(std::clamp(available - 2, 1, 4));
        }
        for (std::uint32_t i = 0; i < threads; ++i) {
            m_impl->threads.emplace_back([impl = m_impl.get()](std::stop_token stop) { impl->Run(stop); });
        }
    }

    TileGenerator::~TileGenerator() {
        // Les jthread demandent l'arrêt (réveillant l'attente) puis attendent leur fil : une tuile en cours finit.
        m_impl->threads.clear();
    }

    std::uint32_t TileGenerator::GetThreadCount() const {
        return static_cast<std::uint32_t>(m_impl->threads.size());
    }

    void TileGenerator::TakeWaiting(std::vector<Job>& waiting) {
        std::lock_guard lock(m_impl->mutex);
        for (Job& job : m_impl->queue) {
            waiting.push_back(std::move(job));
        }
        m_impl->queue.clear();
    }

    void TileGenerator::Submit(std::vector<Job>& jobs) {
        if (jobs.empty()) {
            return;
        }
        {
            std::lock_guard lock(m_impl->mutex);
            for (Job& job : jobs) {
                m_impl->queue.push_back(std::move(job));
            }
        }
        jobs.clear();
        m_impl->wake.notify_all();
    }

    void TileGenerator::Collect(std::vector<Result>& results, std::size_t max) {
        std::lock_guard lock(m_impl->mutex);
        const std::size_t count = std::min(max, m_impl->done.size());
        for (std::size_t i = 0; i < count; ++i) {
            results.push_back(std::move(m_impl->done[i]));
        }
        m_impl->done.erase(m_impl->done.begin(), m_impl->done.begin() + static_cast<std::ptrdiff_t>(count));
    }
}
