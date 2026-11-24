#include "Flux/Planetology/ErosionMap.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Planetology/Planet.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <numbers>
#include <span>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace Flux::Planetology {
    namespace {
        // Change dès que le calcul change : les cartes en cache d'avant sont alors refaites.
        constexpr std::uint32_t MethodVersion = 2;
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

    std::uint32_t ErosionMap::GetLevel(const Terrain& terrain, const ErosionSettings& settings) {
        // Le niveau dont l'écart des sommets est le plus proche de cellSize (à un facteur √2 près).
        const double faceEdge = 0.5 * std::numbers::pi * terrain.GetRadius();
        const double target = std::max(settings.cellSize, 1.0) * std::numbers::sqrt2;
        std::uint32_t level = 0;
        while (level < MaxLevel && faceEdge / static_cast<double>(TileQuads << level) > target) {
            ++level;
        }
        return level;
    }

    std::uint64_t ErosionMap::GetKey(const Terrain& terrain, const ErosionSettings& settings) {
        Hash hash;
        hash.Add(MethodVersion);
        hash.Add(GetLevel(terrain, settings));
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

    void ErosionMap::SetLevel(std::uint32_t level) {
        const std::uint32_t n = TileQuads << level;
        const std::uint32_t m = n + 1;
        m_level = level;
        m_resolution = n;
        m_tangents.resize(m);
        for (std::uint32_t i = 0; i < m; ++i) {
            m_tangents[i] = Tangent((-1.0 + 2.0 * static_cast<double>(i) / static_cast<double>(n)) * QuarterPi);
        }
        // Représentant de chaque point du bord : le plus petit index parmi ses copies, cherchées sur les six faces
        // (une arête en a deux, un coin trois). La direction du point tombe exactement sur un point de l'autre face.
        struct Copy
        {
            std::uint32_t face, i, j;
        };
        std::vector<std::array<Copy, 3>> copies(6ull * 4 * m);
        std::vector<std::uint8_t> copyCount(6ull * 4 * m, 0);
        m_borderCanonical.assign(6ull * 4 * m, NoNode);
        m_borderNeighbors.assign(6ull * 4 * m * 8, NoNode);
        const auto forEachBorder = [&](const auto& work) {
            for (std::uint32_t face = 0; face < 6; ++face) {
                for (std::uint32_t k = 0; k < 4 * m; ++k) {
                    const std::uint32_t i = k < m ? k : (k < 2 * m ? k - m : (k < 3 * m ? 0 : n));
                    const std::uint32_t j = k < m ? 0 : (k < 2 * m ? n : k % m);
                    if (BorderSlot(face, i, j) == face * 4 * m + k) { // un coin n'est vu qu'une fois
                        work(face, i, j, face * 4 * m + k);
                    }
                }
            }
        };
        forEachBorder([&](std::uint32_t face, std::uint32_t i, std::uint32_t j, std::uint32_t slot) {
            const Math::Vector3d direction = GetNodeDirection(GetNode(face, i, j));
            std::uint32_t best = GetNode(face, i, j);
            for (std::uint32_t other = 0; other < 6 && copyCount[slot] < 3; ++other) {
                const double along = Math::Dot(direction, GetFaceNormal(other));
                if (along <= 1.0e-9) {
                    continue;
                }
                const double a = Math::Dot(direction, GetFaceAxisS(other)) / along;
                const double b = Math::Dot(direction, GetFaceAxisT(other)) / along;
                if (std::abs(a) > 1.0 + 1.0e-9 || std::abs(b) > 1.0 + 1.0e-9) {
                    continue;
                }
                const double u = (std::atan(a) / QuarterPi + 1.0) * 0.5 * static_cast<double>(n);
                const double v = (std::atan(b) / QuarterPi + 1.0) * 0.5 * static_cast<double>(n);
                const auto ci = static_cast<std::uint32_t>(std::clamp(std::llround(u), 0ll, static_cast<long long>(n)));
                const auto cj = static_cast<std::uint32_t>(std::clamp(std::llround(v), 0ll, static_cast<long long>(n)));
                copies[slot][copyCount[slot]++] = {other, ci, cj};
                best = std::min(best, GetNode(other, ci, cj));
            }
            m_borderCanonical[slot] = best;
        });
        // Voisins d'un représentant : ceux de chacune de ses copies dans leur face, ramenés à leurs représentants.
        // Le voisinage est ainsi symétrique, d'une face à l'autre comme dans une face.
        forEachBorder([&](std::uint32_t face, std::uint32_t i, std::uint32_t j, std::uint32_t slot) {
            const std::uint32_t node = GetNode(face, i, j);
            if (m_borderCanonical[slot] != node) {
                return;
            }
            std::uint32_t* out = &m_borderNeighbors[8ull * slot];
            std::uint32_t found = 0;
            for (std::uint32_t c = 0; c < copyCount[slot]; ++c) {
                const Copy& copy = copies[slot][c];
                for (int d = 0; d < 8; ++d) {
                    const std::int64_t ni = static_cast<std::int64_t>(copy.i) + OffsetI[d];
                    const std::int64_t nj = static_cast<std::int64_t>(copy.j) + OffsetJ[d];
                    if (ni < 0 || nj < 0 || ni > n || nj > n) {
                        continue;
                    }
                    const std::uint32_t other = Canonical(copy.face, static_cast<std::uint32_t>(ni), static_cast<std::uint32_t>(nj));
                    if (other != node && std::find(out, out + found, other) == out + found && found < 8) {
                        out[found++] = other;
                    }
                }
            }
        });
    }

    std::uint32_t ErosionMap::BorderSlot(std::uint32_t face, std::uint32_t i, std::uint32_t j) const {
        const std::uint32_t n = m_resolution;
        const std::uint32_t m = n + 1;
        const std::uint32_t k = j == 0 ? i : (j == n ? m + i : (i == 0 ? 2 * m + j : 3 * m + j));
        return face * 4 * m + k;
    }

    std::uint32_t ErosionMap::GetCanonical(std::uint32_t node) const {
        const std::uint32_t m = GetSide();
        return Canonical(node / (m * m), node % m, (node / m) % m);
    }

    Math::Vector3d ErosionMap::GetNodeDirection(std::uint32_t node) const {
        const std::uint32_t m = GetSide();
        const std::uint32_t face = node / (m * m);
        return Math::Normalize(GetFaceNormal(face) + GetFaceAxisS(face) * m_tangents[node % m]
                               + GetFaceAxisT(face) * m_tangents[(node / m) % m]);
    }

    void ErosionMap::GetNeighbors(std::uint32_t node, std::uint32_t (&neighbors)[8]) const {
        const std::uint32_t n = m_resolution;
        const std::uint32_t m = n + 1;
        const std::uint32_t face = node / (m * m);
        const std::uint32_t i = node % m;
        const std::uint32_t j = (node / m) % m;
        if (IsBorder(i, j)) {
            std::memcpy(neighbors, &m_borderNeighbors[8ull * BorderSlot(face, i, j)], sizeof(neighbors));
            return;
        }
        for (int d = 0; d < 8; ++d) {
            const auto ni = static_cast<std::uint32_t>(static_cast<std::int32_t>(i) + OffsetI[d]);
            const auto nj = static_cast<std::uint32_t>(static_cast<std::int32_t>(j) + OffsetJ[d]);
            neighbors[d] = Canonical(face, ni, nj);
        }
    }

    std::uint32_t ErosionMap::GetReceiver(std::uint32_t node) const {
        node = GetCanonical(node);
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
        const std::uint32_t level = GetLevel(terrain, settings);
        map.SetLevel(level);
        const std::uint32_t n = map.m_resolution;
        const std::uint32_t count = 6 * map.GetSide() * map.GetSide();
        threads = threads == 0 ? std::max(1u, std::thread::hardware_concurrency()) : threads;
        // Les copies d'un point d'arête ne comptent pas : seul son représentant est calculé, puis recopié.
        std::vector<std::uint8_t> active(count);
        for (std::uint32_t node = 0; node < count; ++node) {
            active[node] = map.GetCanonical(node) == node ? 1 : 0;
        }

        // Hauteurs de départ : la formule sans les octaves plus fines que deux mailles (Terrain::GetOctaves du niveau
        // de tuile dont les sommets sont les points), arrondies.
        const std::uint32_t octaves = terrain.GetOctaves(level);
        // En float : un multiple de 1/16 m y est exact jusqu'à 1 000 km.
        std::vector<float> start(count);
        ParallelRange(count, threads, [&](std::uint32_t begin, std::uint32_t end) {
            for (std::uint32_t node = begin; node < end; ++node) {
                if (!active[node]) {
                    continue;
                }
                start[node] = static_cast<float>(std::nearbyint(terrain.GetReliefHeight(map.GetNodeDirection(node), octaves) / HeightStep) * HeightStep);
            }
        });
        for (std::uint32_t node = 0; node < count; ++node) {
            start[node] = start[map.GetCanonical(node)];
        }

        const double sea = settings.sea ? settings.seaHeight : -std::numeric_limits<double>::infinity();
        std::vector<std::uint8_t> fixed(count, 0);
        std::uint32_t lowest = 0;
        bool anySea = false;
        for (std::uint32_t node = 0; node < count; ++node) {
            if (!active[node]) {
                continue;
            }
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
        map.m_depth = deepest;

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
        std::uint32_t size = 0; // points rangés dans stack (les représentants)

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
                for (std::uint32_t node = 0; node < count; ++node) {
                    visited[node] = active[node] ? 0 : 1;
                }
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
                        if (fixed[node] || !active[node]) {
                            receiver[node] = active[node] ? receiver[node] : node;
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
                size = 0;
                for (std::uint32_t node = 0; node < count; ++node) {
                    if (active[node] && receiver[node] == node) {
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
                for (std::uint32_t k = size; k-- > 0;) {
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
            for (std::uint32_t k = 0; k < size; ++k) {
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
                    if (fixed[node] || !active[node]) {
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
            if (!active[node]) {
                continue;
            }
            map.m_carve[node] = static_cast<float>(height[node] - static_cast<double>(start[node]));
            std::uint8_t direction = Outlet;
            if (receiver[node] != node) {
                std::uint32_t neighbors[8];
                map.GetNeighbors(node, neighbors);
                direction = static_cast<std::uint8_t>(std::find(neighbors, neighbors + 8, receiver[node]) - neighbors);
            }
            map.m_direction[node] = direction;
        }
        for (std::uint32_t node = 0; node < count; ++node) {
            const std::uint32_t canonical = map.GetCanonical(node);
            map.m_carve[node] = map.m_carve[canonical];
            map.m_flow[node] = map.m_flow[canonical];
            map.m_direction[node] = map.m_direction[canonical];
        }
        return map;
    }

    double ErosionMap::SampleFace(std::uint32_t face, double u, double v) const {
        const std::uint32_t n = m_resolution;
        const std::uint32_t m = n + 1;
        const float* data = &m_carve[static_cast<std::size_t>(face) * m * m];
        const double size = static_cast<double>(n);
        u = std::clamp(u, 0.0, size);
        v = std::clamp(v, 0.0, size);
        const auto i0 = static_cast<std::uint32_t>(std::min(std::floor(u), size - 1.0));
        const auto j0 = static_cast<std::uint32_t>(std::min(std::floor(v), size - 1.0));
        const double fu = u - static_cast<double>(i0);
        const double fv = v - static_cast<double>(j0);
        const auto at = [&](std::uint32_t i, std::uint32_t j) { return static_cast<double>(data[j * m + i]); };
        const double bilinear = (at(i0, j0) * (1.0 - fu) + at(i0 + 1, j0) * fu) * (1.0 - fv)
                              + (at(i0, j0 + 1) * (1.0 - fu) + at(i0 + 1, j0 + 1) * fu) * fv;
        // Distance au bord de la face (en mailles) : bilinéaire sur la première maille, Catmull-Rom à partir de la
        // deuxième (ses 4 x 4 points restent alors dans la face).
        const double edge = std::min({u, size - u, v, size - v});
        const double x = std::clamp(edge - 1.0, 0.0, 1.0);
        const double blend = x * x * (3.0 - 2.0 * x);
        double carve = bilinear;
        if (blend > 0.0) {
            const auto weights = [](double t, double (&w)[4]) {
                const double t2 = t * t;
                const double t3 = t2 * t;
                w[0] = 0.5 * (-t3 + 2.0 * t2 - t);
                w[1] = 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0);
                w[2] = 0.5 * (-3.0 * t3 + 4.0 * t2 + t);
                w[3] = 0.5 * (t3 - t2);
            };
            double wu[4];
            double wv[4];
            weights(fu, wu);
            weights(fv, wv);
            double cubic = 0.0;
            for (std::uint32_t b = 0; b < 4; ++b) {
                double row = 0.0;
                for (std::uint32_t a = 0; a < 4; ++a) {
                    row += wu[a] * at(i0 + a - 1, j0 + b - 1);
                }
                cubic += wv[b] * row;
            }
            carve = bilinear + (cubic - bilinear) * blend;
        }
        return std::clamp(carve, -m_depth, 0.0);
    }

    void ErosionMap::SampleGrid(std::uint32_t face, std::span<const double> u, std::span<const double> v, float* out) const {
        // Comme SampleFace en chaque point, mais Catmull-Rom séparable loin des bords (deux mailles et plus) : les
        // quatre rangées de points d'une ligne sont combinées une fois pour toutes ses colonnes.
        const std::uint32_t n = m_resolution;
        const std::uint32_t m = n + 1;
        const double size = static_cast<double>(n);
        const float* data = &m_carve[static_cast<std::size_t>(face) * m * m];
        const auto weights = [](double t, std::array<double, 4>& w) {
            const double t2 = t * t;
            const double t3 = t2 * t;
            w = {0.5 * (-t3 + 2.0 * t2 - t), 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0), 0.5 * (-3.0 * t3 + 4.0 * t2 + t), 0.5 * (t3 - t2)};
        };
        thread_local std::vector<std::uint32_t> columns;
        thread_local std::vector<std::array<double, 4>> columnWeights;
        thread_local std::vector<double> rows;
        columns.resize(u.size());
        columnWeights.resize(u.size());
        std::uint32_t first = m;
        std::uint32_t last = 0;
        for (std::size_t i = 0; i < u.size(); ++i) {
            const double x = std::clamp(u[i], 0.0, size);
            columns[i] = static_cast<std::uint32_t>(std::min(std::floor(x), size - 1.0));
            weights(x - static_cast<double>(columns[i]), columnWeights[i]);
            if (u[i] >= 2.0 && u[i] <= size - 2.0) {
                first = std::min(first, columns[i] - 1);
                last = std::max(last, columns[i] + 2);
            }
        }
        rows.resize(first <= last ? last - first + 1 : 0);
        for (std::size_t j = 0; j < v.size(); ++j) {
            if (v[j] < 0.0 || v[j] > size) {
                continue;
            }
            const auto j0 = static_cast<std::uint32_t>(std::min(std::floor(v[j]), size - 1.0));
            std::array<double, 4> wv;
            weights(v[j] - static_cast<double>(j0), wv);
            const bool inner = v[j] >= 2.0 && v[j] <= size - 2.0;
            if (inner) {
                for (std::uint32_t x = first; x <= last && first <= last; ++x) {
                    double sum = 0.0;
                    for (std::uint32_t b = 0; b < 4; ++b) {
                        sum += wv[b] * static_cast<double>(data[(j0 + b - 1) * m + x]);
                    }
                    rows[x - first] = sum;
                }
            }
            for (std::size_t i = 0; i < u.size(); ++i) {
                if (u[i] < 0.0 || u[i] > size) {
                    continue;
                }
                if (!inner || u[i] < 2.0 || u[i] > size - 2.0) {
                    out[j * u.size() + i] = static_cast<float>(SampleFace(face, u[i], v[j]));
                    continue;
                }
                const double* row = &rows[columns[i] - 1 - first];
                const std::array<double, 4>& wu = columnWeights[i];
                const double carve = wu[0] * row[0] + wu[1] * row[1] + wu[2] * row[2] + wu[3] * row[3];
                out[j * u.size() + i] = static_cast<float>(std::clamp(carve, -m_depth, 0.0));
            }
        }
    }

    void ErosionMap::Locate(const Math::Vector3d& direction, std::uint32_t& face, double& u, double& v) const {
        double s = 0.0;
        double t = 0.0;
        FaceFromDirection(direction, face, s, t);
        const double size = static_cast<double>(m_resolution);
        u = std::clamp((s + 1.0) * 0.5 * size, 0.0, size);
        v = std::clamp((t + 1.0) * 0.5 * size, 0.0, size);
    }

    double ErosionMap::GetCarve(const Math::Vector3d& direction) const {
        if (IsEmpty()) {
            return 0.0;
        }
        std::uint32_t face = 0;
        double u = 0.0;
        double v = 0.0;
        Locate(direction, face, u, v);
        return SampleFace(face, u, v);
    }

    ErosionMap::Sample ErosionMap::Get(const Math::Vector3d& direction) const {
        if (IsEmpty()) {
            return {};
        }
        std::uint32_t face = 0;
        double u = 0.0;
        double v = 0.0;
        Locate(direction, face, u, v);
        Sample sample;
        sample.carve = SampleFace(face, u, v);
        const double size = static_cast<double>(m_resolution);
        const auto i0 = static_cast<std::uint32_t>(std::min(std::floor(u), size - 1.0));
        const auto j0 = static_cast<std::uint32_t>(std::min(std::floor(v), size - 1.0));
        const double fu = u - static_cast<double>(i0);
        const double fv = v - static_cast<double>(j0);
        for (std::uint32_t k = 0; k < 4; ++k) {
            const double w = ((k & 1) ? fu : 1.0 - fu) * ((k >> 1) ? fv : 1.0 - fv);
            sample.flow += w * m_flow[GetNode(face, i0 + (k & 1), j0 + (k >> 1))];
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
            out.write(reinterpret_cast<const char*>(&m_level), sizeof(m_level));
            out.write(reinterpret_cast<const char*>(&m_depth), sizeof(m_depth));
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
        std::uint32_t level = 0;
        double depth = 0.0;
        in.read(reinterpret_cast<char*>(header), sizeof(header));
        in.read(reinterpret_cast<char*>(&storedKey), sizeof(storedKey));
        in.read(reinterpret_cast<char*>(&level), sizeof(level));
        in.read(reinterpret_cast<char*>(&depth), sizeof(depth));
        if (!in || header[0] != FileMagic || header[1] != MethodVersion || storedKey != key || level > MaxLevel || !(depth >= 0.0)) {
            return false;
        }
        const std::size_t side = (TileQuads << level) + 1;
        const std::size_t count = 6 * side * side;
        std::vector<float> carve(count);
        std::vector<float> flow(count);
        std::vector<std::uint8_t> direction(count);
        in.read(reinterpret_cast<char*>(carve.data()), static_cast<std::streamsize>(count * sizeof(float)));
        in.read(reinterpret_cast<char*>(flow.data()), static_cast<std::streamsize>(count * sizeof(float)));
        in.read(reinterpret_cast<char*>(direction.data()), static_cast<std::streamsize>(count));
        if (!in || in.peek() != std::ifstream::traits_type::eof()) {
            return false;
        }
        SetLevel(level);
        m_key = key;
        m_depth = depth;
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

    namespace {
        // Cartes demandées par Request, partagées par la simulation et le rendu (comme un cache d'assets) : la même
        // planète n'est calculée qu'une fois.
        struct Requests
        {
            struct Entry
            {
                std::shared_future<std::shared_ptr<const ErosionMap>> map;
                std::uint64_t last = 0;
            };
            std::mutex mutex;
            std::unordered_map<std::uint64_t, Entry> entries;
            std::uint64_t clock = 0;
        };
        Requests& GetRequests() {
            static Requests requests;
            return requests;
        }
        constexpr std::size_t KeptMaps = 4;
    }

    std::shared_ptr<const ErosionMap> ErosionMap::Request(const Terrain& terrain, const ErosionSettings& settings, bool wait) {
        const std::uint64_t key = GetKey(terrain, settings);
        Requests& requests = GetRequests();
        std::shared_future<std::shared_ptr<const ErosionMap>> map;
        {
            std::lock_guard lock(requests.mutex);
            auto [entry, added] = requests.entries.try_emplace(key);
            if (added) {
                entry->second.map = std::async(std::launch::async, [terrain, settings] {
                    return std::make_shared<const ErosionMap>(
                        LoadOrBuild(terrain, settings, Platform::GetExecutableDirectory() / "Cache"));
                }).share();
            }
            entry->second.last = ++requests.clock;
            map = entry->second.map;
            // Au-delà de KeptMaps, la plus ancienne prête est oubliée (ceux qui l'utilisent la gardent).
            while (requests.entries.size() > KeptMaps) {
                auto oldest = requests.entries.end();
                for (auto it = requests.entries.begin(); it != requests.entries.end(); ++it) {
                    const bool ready = it->second.map.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                    if (it->first != key && ready && (oldest == requests.entries.end() || it->second.last < oldest->second.last)) {
                        oldest = it;
                    }
                }
                if (oldest == requests.entries.end()) {
                    break;
                }
                requests.entries.erase(oldest);
            }
        }
        if (wait || map.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            return map.get();
        }
        return nullptr;
    }
}
