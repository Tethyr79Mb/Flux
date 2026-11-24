#include "Flux/Planetology/ErosionMap.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Planetology/Planet.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <numbers>
#include <system_error>
#include <thread>

namespace Flux::Planetology {
    namespace {
        // Change dès que le calcul change : les cartes en cache d'avant sont alors refaites.
        constexpr std::uint32_t MethodVersion = 1;
        constexpr std::uint32_t FileMagic = 0x52455846u; // « FXER »
        constexpr double QuarterPi = std::numbers::pi / 4.0;
        // Arrondi des hauteurs de départ (m) : efface l'écart d'exp, pow ou sin d'une bibliothèque à l'autre.
        constexpr double HeightStep = 1.0 / 16.0;
        // Pente minimale (m par maille) donnée à l'eau d'un lac ou d'un replat, pour qu'elle sache où aller.
        constexpr double FlatStep = 1.0e-3;
        // Creusement par itération d'un cours d'eau : facteur de la racine de son débit (réglage strength = 1).
        constexpr double BaseErosion = 0.005;
        // Passes de creusement entre deux calculs des chemins de l'eau.
        constexpr std::uint32_t RouteEvery = 4;

        constexpr int OffsetI[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
        constexpr int OffsetJ[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

        // tan(x) par séries (|x| < 0,9) : std::tan n'arrondit pas pareil sous MSVC et sous glibc, or la position
        // des mailles décide des pentes, donc du chemin de l'eau.
        double Tangent(double x) {
            const double x2 = x * x;
            double sine = x;
            double cosine = 1.0;
            double sineTerm = x;
            double cosineTerm = 1.0;
            for (int k = 1; k <= 14; ++k) {
                sineTerm *= -x2 / static_cast<double>((2 * k) * (2 * k + 1));
                cosineTerm *= -x2 / static_cast<double>((2 * k - 1) * (2 * k));
                sine += sineTerm;
                cosine += cosineTerm;
            }
            return sine / cosine;
        }

        // Partage [0, count) entre des fils ; chaque élément n'est écrit que par son fil : même résultat quel que
        // soit leur nombre.
        template <typename Work>
        void ParallelRange(std::uint32_t count, unsigned threads, const Work& work) {
            threads = std::max(1u, std::min(threads, count / 4096u + 1u));
            if (threads == 1) {
                work(0u, count);
                return;
            }
            std::vector<std::jthread> pool;
            pool.reserve(threads - 1);
            const std::uint32_t slice = (count + threads - 1) / threads;
            for (unsigned k = 1; k < threads; ++k) {
                const std::uint32_t begin = std::min(count, k * slice);
                const std::uint32_t end = std::min(count, begin + slice);
                pool.emplace_back([&work, begin, end] { work(begin, end); });
            }
            work(0u, std::min(count, slice));
        }

        struct Hash
        {
            std::uint64_t value = 0xcbf29ce484222325ull;

            void Bytes(const void* data, std::size_t size) {
                const auto* bytes = static_cast<const std::uint8_t*>(data);
                for (std::size_t k = 0; k < size; ++k) {
                    value = (value ^ bytes[k]) * 0x100000001b3ull;
                }
            }
            void Add(double v) { Bytes(&v, sizeof(v)); }
            void Add(std::uint32_t v) { Bytes(&v, sizeof(v)); }
        };
    }

    std::uint32_t ErosionMap::GetResolution(const Terrain& terrain, const ErosionSettings& settings) {
        const double faceEdge = 0.5 * std::numbers::pi * terrain.GetRadius();
        const double cells = std::round(faceEdge / std::max(settings.cellSize, 1.0));
        return static_cast<std::uint32_t>(std::clamp(cells, 16.0, static_cast<double>(MaxResolution)));
    }

    std::uint64_t ErosionMap::GetKey(const Terrain& terrain, const ErosionSettings& settings) {
        Hash hash;
        hash.Add(MethodVersion);
        hash.Add(GetResolution(terrain, settings));
        hash.Add(terrain.GetRadius());
        hash.Add(terrain.GetRelief());
        hash.Bytes(terrain.GetPermutation().data(), terrain.GetPermutation().size());
        hash.Add(terrain.GetScale());
        hash.Add(terrain.GetZoneScale());
        hash.Add(terrain.GetContinentScale());
        hash.Add(terrain.GetCraterScale());
        hash.Add(terrain.GetCraterOctaves());
        const Terrain::ShapeConstants& shape = terrain.GetShapeConstants();
        for (const double v : {shape.chainStart, shape.chainEnd, shape.hillStart, shape.hillEnd, shape.detailPlains,
                               shape.detailHills, shape.detailMountains, shape.hillWeight, shape.gullyAmplitude,
                               shape.erosion, shape.warpLarge, shape.warpSmall, shape.craterChance, shape.craterGain,
                               shape.craterDepth, shape.craterRim, shape.craterWear, shape.craterComplex}) {
            hash.Add(v);
        }
        hash.Add(shape.peakChance);
        for (const double v : {settings.cellSize, settings.strength, settings.rain, settings.valleyDepth,
                               settings.channelArea, settings.valleySlope, settings.seaHeight}) {
            hash.Add(v);
        }
        hash.Add(settings.iterations);
        hash.Add(settings.sea ? 1u : 0u);
        return hash.value;
    }

    void ErosionMap::SetResolution(std::uint32_t resolution) {
        const std::uint32_t n = resolution;
        m_resolution = n;
        m_tangents.resize(n + 2);
        for (std::uint32_t k = 0; k < n + 2; ++k) {
            const double s = -1.0 + static_cast<double>(2 * static_cast<std::int64_t>(k) - 1) / static_cast<double>(n);
            m_tangents[k] = Tangent(s * QuarterPi);
        }
        // Voisins des bords : d'une face à l'autre, par la direction du point d'à côté. Les mailles de deux faces
        // tombent face à face le long d'une arête ; seul le coin du cube (trois faces) en perd un.
        m_borderNeighbors.assign(6ull * 4 * n * 8, NoNode);
        for (std::uint32_t face = 0; face < 6; ++face) {
            for (std::uint32_t k = 0; k < n; ++k) {
                const std::uint32_t cells[4][2] = {{k, 0}, {k, n - 1}, {0, k}, {n - 1, k}};
                for (const auto& cell : cells) {
                    const std::uint32_t node = (face * n + cell[1]) * n + cell[0];
                    std::uint32_t* out = &m_borderNeighbors[8ull * BorderSlot(face, cell[0], cell[1])];
                    for (int d = 0; d < 8; ++d) {
                        const std::uint32_t other = ResolveNode(face, static_cast<std::int64_t>(cell[0]) + OffsetI[d],
                                                                static_cast<std::int64_t>(cell[1]) + OffsetJ[d]);
                        const bool repeated = other == node || std::find(out, out + d, other) != out + d;
                        out[d] = repeated ? NoNode : other;
                    }
                }
            }
        }
        // Voisinage symétrique (l'eau doit pouvoir repartir par où on l'a trouvée) : au coin, une case libre
        // accueille le voisin qui manque, sinon le lien est retiré des deux côtés.
        const auto neighborsOf = [&](std::uint32_t node) -> std::uint32_t* {
            const std::uint32_t face = node / (n * n);
            const std::uint32_t i = node % n;
            const std::uint32_t j = (node / n) % n;
            const bool border = i == 0 || j == 0 || i == n - 1 || j == n - 1;
            return border ? &m_borderNeighbors[8ull * BorderSlot(face, i, j)] : nullptr;
        };
        for (std::uint32_t face = 0; face < 6; ++face) {
            for (std::uint32_t k = 0; k < 4 * n; ++k) {
                const std::uint32_t i = k < 2 * n ? k % n : (k < 3 * n ? 0 : n - 1);
                const std::uint32_t j = k < n ? 0 : (k < 2 * n ? n - 1 : k % n);
                if (BorderSlot(face, i, j) != face * 4 * n + k) {
                    continue; // coin déjà vu sous une autre case
                }
                const std::uint32_t node = (face * n + j) * n + i;
                std::uint32_t* mine = &m_borderNeighbors[8ull * (face * 4 * n + k)];
                for (int d = 0; d < 8; ++d) {
                    if (mine[d] == NoNode) {
                        continue;
                    }
                    std::uint32_t* theirs = neighborsOf(mine[d]);
                    if (!theirs || std::find(theirs, theirs + 8, node) != theirs + 8) {
                        continue; // voisin intérieur (symétrique par construction) ou déjà réciproque
                    }
                    std::uint32_t* free = std::find(theirs, theirs + 8, NoNode);
                    if (free != theirs + 8) {
                        *free = node;
                    } else {
                        mine[d] = NoNode;
                    }
                }
            }
        }
    }

    std::uint32_t ErosionMap::BorderSlot(std::uint32_t face, std::uint32_t i, std::uint32_t j) const {
        const std::uint32_t n = m_resolution;
        const std::uint32_t k = j == 0 ? i : (j == n - 1 ? n + i : (i == 0 ? 2 * n + j : 3 * n + j));
        return face * 4 * n + k;
    }

    std::uint32_t ErosionMap::ResolveNode(std::uint32_t face, std::int64_t i, std::int64_t j) const {
        const std::int64_t n = m_resolution;
        if (i >= 0 && j >= 0 && i < n && j < n) {
            return static_cast<std::uint32_t>((face * n + j) * n + i);
        }
        // Le centre d'à côté tombe au centre d'une maille de la face voisine : loin de tout arrondi.
        const Math::Vector3d direction = Math::Normalize(GetFaceNormal(face) + GetFaceAxisS(face) * m_tangents[static_cast<std::size_t>(i + 1)]
                                                         + GetFaceAxisT(face) * m_tangents[static_cast<std::size_t>(j + 1)]);
        std::uint32_t other = 0;
        double s = 0.0;
        double t = 0.0;
        FaceFromDirection(direction, other, s, t);
        const auto cell = [n](double u) {
            return std::clamp(static_cast<std::int64_t>(std::floor((u + 1.0) * 0.5 * static_cast<double>(n))), std::int64_t{0}, n - 1);
        };
        return static_cast<std::uint32_t>((other * n + cell(t)) * n + cell(s));
    }

    Math::Vector3d ErosionMap::GetNodeDirection(std::uint32_t node) const {
        const std::uint32_t n = m_resolution;
        const std::uint32_t face = node / (n * n);
        const std::uint32_t i = node % n;
        const std::uint32_t j = (node / n) % n;
        return Math::Normalize(GetFaceNormal(face) + GetFaceAxisS(face) * m_tangents[i + 1] + GetFaceAxisT(face) * m_tangents[j + 1]);
    }

    void ErosionMap::GetNeighbors(std::uint32_t node, std::uint32_t (&neighbors)[8]) const {
        const std::uint32_t n = m_resolution;
        const std::uint32_t i = node % n;
        const std::uint32_t j = (node / n) % n;
        if (i == 0 || j == 0 || i == n - 1 || j == n - 1) {
            std::memcpy(neighbors, &m_borderNeighbors[8ull * BorderSlot(node / (n * n), i, j)], sizeof(neighbors));
            return;
        }
        for (int d = 0; d < 8; ++d) {
            neighbors[d] = static_cast<std::uint32_t>(static_cast<std::int64_t>(node) + OffsetI[d] + static_cast<std::int64_t>(OffsetJ[d]) * n);
        }
    }

    std::uint32_t ErosionMap::GetReceiver(std::uint32_t node) const {
        const std::uint8_t direction = m_direction[node];
        if (direction == Outlet) {
            return node;
        }
        std::uint32_t neighbors[8];
        GetNeighbors(node, neighbors);
        return neighbors[direction];
    }

    ErosionMap ErosionMap::Build(const Terrain& terrain, const ErosionSettings& settings, unsigned threads) {
        ErosionMap map;
        map.m_key = GetKey(terrain, settings);
        const std::uint32_t n = GetResolution(terrain, settings);
        map.SetResolution(n);
        const std::uint32_t count = 6 * n * n;
        threads = threads == 0 ? std::max(1u, std::thread::hardware_concurrency()) : threads;

        // Hauteurs de départ : la formule sans les octaves plus fines que deux mailles (Terrain::GetOctaves du niveau
        // de tuile au même écart de sommets), arrondies.
        std::uint32_t level = 0;
        while ((TileQuads << (level + 1)) <= n) {
            ++level;
        }
        const std::uint32_t octaves = terrain.GetOctaves(level);
        // En float : un multiple de 1/16 m y est exact jusqu'à 1 000 km.
        std::vector<float> start(count);
        ParallelRange(count, threads, [&](std::uint32_t begin, std::uint32_t end) {
            for (std::uint32_t node = begin; node < end; ++node) {
                start[node] = static_cast<float>(std::nearbyint(terrain.GetHeight(map.GetNodeDirection(node), octaves) / HeightStep) * HeightStep);
            }
        });

        const double sea = settings.sea ? settings.seaHeight : -std::numeric_limits<double>::infinity();
        std::vector<std::uint8_t> fixed(count, 0);
        std::uint32_t lowest = 0;
        bool anySea = false;
        for (std::uint32_t node = 0; node < count; ++node) {
            fixed[node] = start[node] < sea ? 1 : 0;
            anySea = anySea || fixed[node] != 0;
            lowest = start[node] < start[lowest] ? node : lowest;
        }
        if (!anySea) {
            fixed[lowest] = 1; // planète sans mer : toute l'eau finit au point le plus bas (les creux font des lacs)
        }
        // Le creusement s'arrête à valleyDepth sous le départ, et jamais sous la mer (pas de mer nouvelle).
        const double deepest = std::max(settings.valleyDepth, 0.0) * terrain.GetRelief();
        const double erosion = BaseErosion * std::max(settings.strength, 0.0);
        const double channel = std::sqrt(std::max(settings.channelArea, 0.0));
        const double rain = std::max(settings.rain, 1.0e-3);
        const double slope = Tangent(std::clamp(settings.valleySlope, 5.0, 50.0) * (std::numbers::pi / 180.0));
        const double cellSize = 0.5 * std::numbers::pi * terrain.GetRadius() / static_cast<double>(n);

        std::vector<double> height(start.begin(), start.end());
        std::vector<double> slumped(count);
        std::vector<double> filled(count);
        std::vector<std::uint32_t> receiver(count);
        std::vector<float> distance(count);
        std::vector<float> flow(count);
        std::vector<std::uint32_t> donorStart(count + 1);
        std::vector<std::uint32_t> donors(count);
        std::vector<std::uint32_t> stack(count);
        std::vector<std::uint8_t> visited(count);
        std::vector<std::uint32_t> next(count);
        std::vector<std::uint32_t> bucketHead;
        std::vector<std::uint32_t> bucketTail;
        std::vector<std::uint32_t> pit;

        const std::uint32_t passes = std::max(settings.iterations, 1u);
        for (std::uint32_t pass = 0; pass < passes; ++pass) {
            // Les chemins de l'eau (étapes 1 à 4, l'essentiel du coût) ne sont refaits que toutes les RouteEvery
            // passes : entre deux, le creusement suit l'arbre précédent, toujours valable (sans boucle).
            if (pass % RouteEvery == 0) {
                // 1. Remplissage des creux (Barnes, Lehman et Mulla 2014) depuis la mer : la surface de l'eau, qui
                // descend toujours vers une sortie ; un lac en garde une pente infime. La maille qui en découvre une
                // autre en est le premier receveur possible. File par paniers de HeightStep (de la plus basse à la plus
                // haute, dans l'ordre d'arrivée dans un panier) : sans tri, la surface d'un lac est juste à HeightStep
                // près.
                double low = height[0];
                double high = height[0];
                for (const double h : height) {
                    low = std::min(low, h);
                    high = std::max(high, h);
                }
                const std::size_t bucketCount = static_cast<std::size_t>((high - low) / HeightStep) + 2;
                bucketHead.assign(bucketCount, NoNode);
                bucketTail.assign(bucketCount, NoNode);
                const auto bucketOf = [&](double h) { return static_cast<std::size_t>((h - low) / HeightStep); };
                const auto push = [&](std::size_t bucket, std::uint32_t node) {
                    next[node] = NoNode;
                    if (bucketTail[bucket] == NoNode) {
                        bucketHead[bucket] = node;
                    } else {
                        next[bucketTail[bucket]] = node;
                    }
                    bucketTail[bucket] = node;
                };
                std::fill(visited.begin(), visited.end(), std::uint8_t{0});
                pit.clear();
                for (std::uint32_t node = 0; node < count; ++node) {
                    if (fixed[node]) {
                        visited[node] = 1;
                        filled[node] = height[node];
                        receiver[node] = node;
                        push(bucketOf(height[node]), node);
                    }
                }
                std::size_t bucket = 0;
                std::size_t pitHead = 0;
                while (true) {
                    std::uint32_t node;
                    if (pitHead < pit.size()) {
                        node = pit[pitHead++];
                    } else {
                        pit.clear();
                        pitHead = 0;
                        while (bucket < bucketCount && bucketHead[bucket] == NoNode) {
                            ++bucket;
                        }
                        if (bucket == bucketCount) {
                            break;
                        }
                        node = bucketHead[bucket];
                        bucketHead[bucket] = next[node];
                        if (bucketHead[bucket] == NoNode) {
                            bucketTail[bucket] = NoNode;
                        }
                    }
                    std::uint32_t neighbors[8];
                    map.GetNeighbors(node, neighbors);
                    for (const std::uint32_t other : neighbors) {
                        if (other == NoNode || visited[other]) {
                            continue;
                        }
                        visited[other] = 1;
                        receiver[other] = node;
                        const double spill = filled[node] + FlatStep;
                        if (height[other] <= spill) {
                            filled[other] = spill;
                            pit.push_back(other);
                        } else {
                            filled[other] = height[other];
                            push(std::max(bucketOf(height[other]), bucket), other);
                        }
                    }
                }

                // 2. Receveur : le voisin à la plus forte pente vers le bas de la surface de l'eau (8 directions, écart
                // nominal des mailles), puis la vraie distance jusqu'à lui.
                ParallelRange(count, threads, [&](std::uint32_t begin, std::uint32_t end) {
                    for (std::uint32_t node = begin; node < end; ++node) {
                        if (fixed[node]) {
                            distance[node] = 1.0f;
                            continue;
                        }
                        std::uint32_t neighbors[8];
                        map.GetNeighbors(node, neighbors);
                        std::uint32_t best = receiver[node];
                        double bestSlope = -1.0;
                        for (int d = 0; d < 8; ++d) {
                            const std::uint32_t other = neighbors[d];
                            if (other == NoNode || !(filled[other] < filled[node])) {
                                continue;
                            }
                            const double drop = (filled[node] - filled[other]) * (d == 1 || d == 3 || d == 4 || d == 6 ? 1.0 : std::numbers::sqrt2 * 0.5);
                            if (drop > bestSlope || (drop == bestSlope && other < best)) {
                                bestSlope = drop;
                                best = other;
                            }
                        }
                        receiver[node] = best;
                        distance[node] = static_cast<float>(terrain.GetRadius() * Math::Length(map.GetNodeDirection(node) - map.GetNodeDirection(best)));
                    }
                });

                // 3. Ordre de l'aval vers l'amont (Braun et Willett 2013) : chaque maille après son receveur.
                std::fill(donorStart.begin(), donorStart.end(), 0u);
                for (std::uint32_t node = 0; node < count; ++node) {
                    if (receiver[node] != node) {
                        ++donorStart[receiver[node] + 1];
                    }
                }
                for (std::uint32_t node = 0; node < count; ++node) {
                    donorStart[node + 1] += donorStart[node];
                }
                {
                    std::vector<std::uint32_t>& cursor = stack; // emprunté le temps de ranger les donneurs
                    std::copy(donorStart.begin(), donorStart.end() - 1, cursor.begin());
                    for (std::uint32_t node = 0; node < count; ++node) {
                        if (receiver[node] != node) {
                            donors[cursor[receiver[node]]++] = node;
                        }
                    }
                }
                std::uint32_t size = 0;
                for (std::uint32_t node = 0; node < count; ++node) {
                    if (receiver[node] == node) {
                        stack[size++] = node;
                    }
                }
                for (std::uint32_t k = 0; k < size; ++k) {
                    const std::uint32_t node = stack[k];
                    for (std::uint32_t d = donorStart[node]; d < donorStart[node + 1]; ++d) {
                        stack[size++] = donors[d];
                    }
                }

                // 4. Débit : chaque maille reçoit la pluie de tout son amont.
                std::fill(flow.begin(), flow.end(), static_cast<float>(rain));
                for (std::uint32_t k = count; k-- > 0;) {
                    const std::uint32_t node = stack[k];
                    if (receiver[node] != node) {
                        flow[receiver[node]] += flow[node];
                    }
                }
            }

            // 5. Creusement implicite (loi de puissance du courant, m = 1/2, n = 1), de l'aval vers l'amont : la maille
            // rejoint son receveur d'autant plus vite que son débit est fort. Sous channelArea mailles drainées, rien
            // (les crêtes et les sommets restent tels que la formule les fait). Dans un lac (plus bas que la sortie),
            // rien non plus.
            for (std::uint32_t k = 0; k < count; ++k) {
                const std::uint32_t node = stack[k];
                const std::uint32_t down = receiver[node];
                if (down == node || height[node] <= height[down]) {
                    continue;
                }
                const double power = std::max(std::sqrt(static_cast<double>(flow[node])) - channel, 0.0);
                const double f = erosion * power * cellSize / static_cast<double>(distance[node]);
                const double carved = (height[node] + f * height[down]) / (1.0 + f);
                const double first = start[node];
                height[node] = std::max({carved, first - deepest, std::min(first, sea)});
            }

            // 6. Versants : un lit creusé tout seul serait une tranchée d'une maille. Une maille plus raide que
            // valleySlope au-dessus d'un voisin s'éboule jusqu'à cette pente, seulement si c'est le creusement qui l'a
            // rendue raide (pente de départ plus douce) : la vallée s'évase en V, les montagnes gardent leurs pentes.
            ParallelRange(count, threads, [&](std::uint32_t begin, std::uint32_t end) {
                for (std::uint32_t node = begin; node < end; ++node) {
                    double h = height[node];
                    if (fixed[node]) {
                        slumped[node] = h;
                        continue;
                    }
                    std::uint32_t neighbors[8];
                    map.GetNeighbors(node, neighbors);
                    for (int d = 0; d < 8; ++d) {
                        const std::uint32_t other = neighbors[d];
                        if (other == NoNode) {
                            continue;
                        }
                        const double rise = slope * cellSize * (d == 1 || d == 3 || d == 4 || d == 6 ? 1.0 : std::numbers::sqrt2);
                        if (static_cast<double>(start[node]) - static_cast<double>(start[other]) < rise) {
                            h = std::min(h, height[other] + rise);
                        }
                    }
                    const double first = start[node];
                    slumped[node] = std::max({h, first - deepest, std::min(first, sea)});
                }
            });
            height.swap(slumped);
        }

        map.m_carve.resize(count);
        map.m_flow = std::move(flow);
        map.m_direction.resize(count);
        for (std::uint32_t node = 0; node < count; ++node) {
            map.m_carve[node] = static_cast<float>(height[node] - static_cast<double>(start[node]));
            std::uint8_t direction = Outlet;
            if (receiver[node] != node) {
                std::uint32_t neighbors[8];
                map.GetNeighbors(node, neighbors);
                direction = static_cast<std::uint8_t>(std::find(neighbors, neighbors + 8, receiver[node]) - neighbors);
            }
            map.m_direction[node] = direction;
        }
        return map;
    }

    ErosionMap::Sample ErosionMap::Get(const Math::Vector3d& direction) const {
        if (IsEmpty()) {
            return {};
        }
        std::uint32_t face = 0;
        double s = 0.0;
        double t = 0.0;
        FaceFromDirection(direction, face, s, t);
        const double n = static_cast<double>(m_resolution);
        const double u = std::clamp((s + 1.0) * 0.5 * n - 0.5, -1.0, n);
        const double v = std::clamp((t + 1.0) * 0.5 * n - 0.5, -1.0, n);
        const std::int64_t i = std::min(static_cast<std::int64_t>(std::floor(u)), static_cast<std::int64_t>(m_resolution) - 1);
        const std::int64_t j = std::min(static_cast<std::int64_t>(std::floor(v)), static_cast<std::int64_t>(m_resolution) - 1);
        const double fu = u - static_cast<double>(i);
        const double fv = v - static_cast<double>(j);
        Sample sample;
        for (int k = 0; k < 4; ++k) {
            const std::uint32_t node = ResolveNode(face, i + (k & 1), j + (k >> 1));
            const double w = ((k & 1) ? fu : 1.0 - fu) * ((k >> 1) ? fv : 1.0 - fv);
            sample.carve += w * m_carve[node];
            sample.flow += w * m_flow[node];
        }
        return sample;
    }

    bool ErosionMap::Save(const std::filesystem::path& file) const {
        std::filesystem::path temporary = file;
        temporary += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out) {
                return false;
            }
            const std::uint32_t header[2] = {FileMagic, MethodVersion};
            out.write(reinterpret_cast<const char*>(header), sizeof(header));
            out.write(reinterpret_cast<const char*>(&m_key), sizeof(m_key));
            out.write(reinterpret_cast<const char*>(&m_resolution), sizeof(m_resolution));
            out.write(reinterpret_cast<const char*>(m_carve.data()), static_cast<std::streamsize>(m_carve.size() * sizeof(float)));
            out.write(reinterpret_cast<const char*>(m_flow.data()), static_cast<std::streamsize>(m_flow.size() * sizeof(float)));
            out.write(reinterpret_cast<const char*>(m_direction.data()), static_cast<std::streamsize>(m_direction.size()));
            if (!out) {
                return false;
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, file, error);
        return !error;
    }

    bool ErosionMap::Load(const std::filesystem::path& file, std::uint64_t key) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return false;
        }
        std::uint32_t header[2] = {};
        std::uint64_t storedKey = 0;
        std::uint32_t resolution = 0;
        in.read(reinterpret_cast<char*>(header), sizeof(header));
        in.read(reinterpret_cast<char*>(&storedKey), sizeof(storedKey));
        in.read(reinterpret_cast<char*>(&resolution), sizeof(resolution));
        if (!in || header[0] != FileMagic || header[1] != MethodVersion || storedKey != key || resolution < 16
            || resolution > MaxResolution) {
            return false;
        }
        const std::size_t count = 6ull * resolution * resolution;
        std::vector<float> carve(count);
        std::vector<float> flow(count);
        std::vector<std::uint8_t> direction(count);
        in.read(reinterpret_cast<char*>(carve.data()), static_cast<std::streamsize>(count * sizeof(float)));
        in.read(reinterpret_cast<char*>(flow.data()), static_cast<std::streamsize>(count * sizeof(float)));
        in.read(reinterpret_cast<char*>(direction.data()), static_cast<std::streamsize>(count));
        if (!in || in.peek() != std::ifstream::traits_type::eof()) {
            return false;
        }
        SetResolution(resolution);
        m_key = key;
        m_carve = std::move(carve);
        m_flow = std::move(flow);
        m_direction = std::move(direction);
        return true;
    }

    ErosionMap ErosionMap::LoadOrBuild(const Terrain& terrain, const ErosionSettings& settings,
                                       const std::filesystem::path& cacheDirectory, unsigned threads) {
        const std::uint64_t key = GetKey(terrain, settings);
        const std::filesystem::path file = cacheDirectory / std::format("erosion-{:016x}.bin", key);
        ErosionMap map;
        std::error_code error;
        if (std::filesystem::exists(file, error)) {
            if (map.Load(file, key)) {
                return map;
            }
            Log::Warning("Érosion : cache {} illisible, carte recalculée", file.string());
        }
        map = Build(terrain, settings, threads);
        std::filesystem::create_directories(cacheDirectory, error);
        if (!map.Save(file)) {
            Log::Warning("Érosion : impossible d'enregistrer la carte dans {}", file.string());
        }
        return map;
    }
}
