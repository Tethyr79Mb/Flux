#include "Flux/Graphics/Planet.hpp"

#include <algorithm>
#include <limits>
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
        // Octaves des crêtes des montagnes (elles s'arrêtent plus tôt : elles coûtent autant que le reste).
        constexpr std::uint32_t MaxRidgeOctaves = 8;
        // Relief de détail (bosses, rochers) à partir de l'octave 6 (environ 4 km de longueur d'onde sur une planète
        // de 60 km), x 0,65 par octave (contre 0,5 pour les grandes formes) : le sol reste accidenté de près. Son
        // amplitude (fraction du relief) dépend de l'endroit : presque rien en plaine, plus en collines et en montagne.
        constexpr std::uint32_t DetailFirstOctave = 6;
        constexpr double DetailGain = 0.58;
        constexpr double DetailPlains = 0.012;
        constexpr double DetailHills = 0.03;
        constexpr double DetailMountains = 0.065;

        // Couches du relief (voir GetHeight), dans l'ordre de TileGpuInputs::cells et de shaders/PlanetTile.slang.
        struct Layer
        {
            std::uint32_t first; // octaves [first, last)
            std::uint32_t last;
            double gain;
            Math::Vector3d offset; // sépare les couches (sinon elles suivraient les mêmes formes)
            bool ridged;
        };
        enum LayerIndex { Continents, MountainZone, HillZone, Mountains, Hills, Detail };
        constexpr std::array<Layer, 6> Layers{{
            {0, 5, 0.5, {0.0, 0.0, 0.0}, false},
            {1, 4, 0.5, {31.7, -12.3, 7.1}, false},
            {2, 5, 0.5, {-8.9, 44.2, -19.6}, false},
            {2, 2 + MaxRidgeOctaves, 0.5, {17.3, -41.7, 8.9}, true},
            {4, 9, 0.5, {5.3, 23.9, -33.1}, false},
            {DetailFirstOctave, MaxOctaves, DetailGain, {-23.1, 5.7, 31.3}, false},
        }};
        static_assert([] {
            std::uint32_t count = 0;
            for (const Layer& layer : Layers) {
                count += layer.last - layer.first;
            }
            return count;
        }() == TileGpuInputs::Octaves);

        double SmoothStep(double edge0, double edge1, double x) {
            const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        }

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

    // Somme des octaves [first, min(last, octaves)) d'une couche (octave k : fréquence BaseFrequency x 2^k), chacune
    // `gain` fois plus basse que la précédente, divisée par la somme des amplitudes de toutes ses octaves [first,
    // last) : de -1 à 1 environ (le plus souvent entre -0,5 et 0,5), à la même échelle quel que soit le nombre
    // d'octaves d'une tuile.
    double Terrain::Fbm(const Math::Vector3d& direction, std::uint32_t layerIndex, std::uint32_t octaves) const {
        const Layer& layer = Layers[layerIndex];
        const double gain = layer.gain;
        const bool ridged = layer.ridged;
        const double total = (1.0 - std::pow(gain, static_cast<double>(layer.last - layer.first))) / (1.0 - gain);
        const std::uint32_t last = std::min(layer.last, octaves);
        double sum = 0.0;
        double amplitude = 1.0;
        Math::Vector3d p = direction * (BaseFrequency * std::ldexp(1.0, static_cast<int>(layer.first))) + layer.offset;
        for (std::uint32_t k = layer.first; k < last; ++k) {
            const double n = Noise(p);
            // Crêtes : 1 - |bruit| fait une arête vive là où le bruit passe par 0, au carré pour des vallées larges.
            sum += amplitude * (ridged ? (1.0 - std::abs(n)) * (1.0 - std::abs(n)) : n);
            amplitude *= gain;
            p *= 2.0;
        }
        return sum / total;
    }

    double Terrain::GetHeight(const Math::Vector3d& direction, std::uint32_t octaves) const {
        if (m_relief <= 0.0) {
            return 0.0;
        }
        // Couches, des plus grandes aux plus fines (octave 0 : environ un quart du tour de la planète) :
        // - continents : terres et bassins ;
        const double continents = Fbm(direction, Continents, octaves);
        const double land = SmoothStep(-0.26, 0.06, continents);
        // - où sont les montagnes (des chaînes sur les terres, pas partout) et les collines ;
        const double mountainZone = SmoothStep(0.08, 0.36, Fbm(direction, MountainZone, octaves)) * land;
        const double hillZone = SmoothStep(0.05, 0.4, Fbm(direction, HillZone, octaves));
        // - les reliefs eux-mêmes, calculés seulement où ils comptent (une plaine ne paie pas les crêtes) ;
        const double mountains = mountainZone > 0.0 ? Fbm(direction, Mountains, octaves) : 0.0;
        const double hills = hillZone > 0.0 ? Fbm(direction, Hills, octaves) : 0.0;
        // - le détail fin (bosses, rochers), qui baisse moins vite (DetailGain) : faible dans les plaines.
        const double roughness = DetailPlains + DetailHills * hillZone + DetailMountains * mountainZone;
        const double detail = Fbm(direction, Detail, octaves);
        // Bassins vers -0,4, plaines vers 0,05 à 0,1, montagnes jusqu'au relief.
        const double height = -0.4 + 0.45 * land + 0.15 * continents + 0.85 * mountainZone * mountains
                            + 0.15 * hillZone * hills + roughness * detail;
        return m_relief * std::clamp(height, -1.0, 1.0);
    }

    double Terrain::GetOmittedRelief(std::uint32_t octaves) const {
        // Ce que les octaves de rang >= octaves peuvent encore ajouter, au plus fort (montagnes) : la fin de chaque
        // couche, en fraction de la somme de ses amplitudes.
        const auto tail = [octaves](std::uint32_t first, std::uint32_t last, double gain) {
            if (octaves >= last) {
                return 0.0;
            }
            const auto from = std::max(octaves, first);
            const double all = (1.0 - std::pow(gain, last - first)) / (1.0 - gain);
            return std::pow(gain, from - first) * (1.0 - std::pow(gain, last - from)) / (1.0 - gain) / all;
        };
        const auto layerTail = [&](LayerIndex index) {
            return tail(Layers[index].first, Layers[index].last, Layers[index].gain);
        };
        const double omitted = 0.6 * layerTail(Continents) + 0.85 * layerTail(Mountains) + 0.15 * layerTail(Hills)
                             + (DetailPlains + DetailHills + DetailMountains) * layerTail(Detail);
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
        mesh.lowest = std::numeric_limits<double>::max();
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
                mesh.lowest = std::min(mesh.lowest, Math::Length(positions[w]));
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
                    mesh.lowest = std::min(mesh.lowest, Math::Length(target));
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

    void PrepareTileGpu(const Terrain& terrain, TileKey key, TileGpuInputs& inputs) {
        const Face& f = Faces[key.face % 6];
        const double R = terrain.GetRadius();
        const std::uint32_t octaves = terrain.GetOctaves(key.level);
        const auto x0 = static_cast<std::int64_t>(key.x) * TileQuads;
        const auto y0 = static_cast<std::int64_t>(key.y) * TileQuads;
        constexpr auto half = static_cast<std::int64_t>(TileQuads / 2);
        const auto tangent = [&](std::int64_t index) { return std::tan(GridToFace(index, key.level) * QuarterPi); };
        const double ac = tangent(x0 + half);
        const double bc = tangent(y0 + half);
        const Math::Vector3d vc = f.normal + f.s * ac + f.t * bc;
        const double lc = Math::Length(vc);
        const Math::Vector3d dc = vc / lc; // = DirectionFromFace au centre : le même que mesh.center / R
        const auto float4 = [](const Math::Vector3d& v, double w) {
            return std::array{static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z), static_cast<float>(w)};
        };
        // Profondeur des jupes : comme BuildTile.
        const double edge = Math::Length(DirectionFromFace(key.face, GridToFace(x0 + TileQuads, key.level), GridToFace(y0, key.level))
                                         - DirectionFromFace(key.face, GridToFace(x0, key.level), GridToFace(y0, key.level))) * R;
        const double depth = 0.05 * edge + 2.0 * terrain.GetOmittedRelief(octaves) + 1.0;
        inputs.center = float4(dc, lc);
        inputs.axisN = float4(f.normal, R);
        inputs.axisS = float4(f.s, terrain.GetRelief());
        inputs.axisT = float4(f.t, depth);
        inputs.options = {octaves, key.level > 0 ? terrain.GetOctaves(key.level - 1u) : octaves, key.level, 0};
        for (std::uint32_t i = 0; i < TileGpuInputs::Columns; ++i) {
            const double a = tangent(x0 + static_cast<std::int64_t>(i) - 2);
            const double b = tangent(y0 + static_cast<std::int64_t>(i) - 2);
            inputs.columns[i] = {static_cast<float>(a - ac), static_cast<float>(a + ac), static_cast<float>(b - bc), static_cast<float>(b + bc)};
        }
        // Point de chaque octave au centre de la tuile, calculé comme Fbm (doublé d'une octave à l'autre).
        std::uint32_t octave = 0;
        for (const Layer& layer : Layers) {
            Math::Vector3d p = dc * (BaseFrequency * std::ldexp(1.0, static_cast<int>(layer.first))) + layer.offset;
            for (std::uint32_t k = layer.first; k < layer.last; ++k, ++octave) {
                const Math::Vector3d cell{std::floor(p.x), std::floor(p.y), std::floor(p.z)};
                inputs.cells[octave] = {static_cast<std::int32_t>(static_cast<std::int64_t>(cell.x) & 255),
                                        static_cast<std::int32_t>(static_cast<std::int64_t>(cell.y) & 255),
                                        static_cast<std::int32_t>(static_cast<std::int64_t>(cell.z) & 255), 0};
                inputs.fractions[octave] = float4(p - cell, BaseFrequency * std::ldexp(1.0, static_cast<int>(k)));
                p *= 2.0;
            }
        }
        const auto& permutation = terrain.GetPermutation();
        for (std::uint32_t i = 0; i < inputs.permutation.size(); ++i) {
            inputs.permutation[i] = static_cast<std::uint32_t>(permutation[4 * i]) | (static_cast<std::uint32_t>(permutation[4 * i + 1]) << 8)
                                  | (static_cast<std::uint32_t>(permutation[4 * i + 2]) << 16)
                                  | (static_cast<std::uint32_t>(permutation[4 * i + 3]) << 24);
        }
    }

    void BuildTileSurface(const Terrain& terrain, TileKey key, Math::Vector3d& center, std::vector<Math::Vector3f>& positions) {
        // Mêmes calculs que BuildTile, dans le même ordre : mêmes doubles, donc mêmes floats.
        const double R = terrain.GetRadius();
        const std::uint32_t octaves = terrain.GetOctaves(key.level);
        const auto x0 = static_cast<std::int64_t>(key.x) * TileQuads;
        const auto y0 = static_cast<std::int64_t>(key.y) * TileQuads;
        constexpr auto half = static_cast<std::int64_t>(TileQuads / 2);
        center = DirectionFromFace(key.face, GridToFace(x0 + half, key.level), GridToFace(y0 + half, key.level)) * R;
        positions.resize(TileSide * TileSide);
        for (std::uint32_t j = 0; j < TileSide; ++j) {
            const double t = GridToFace(y0 + j, key.level);
            for (std::uint32_t i = 0; i < TileSide; ++i) {
                const Math::Vector3d d = DirectionFromFace(key.face, GridToFace(x0 + i, key.level), t);
                positions[j * TileSide + i] = Math::Vector3f(d * (R + terrain.GetHeight(d, octaves)) - center);
            }
        }
    }

    TileKey FindTile(const Math::Vector3d& direction, std::uint32_t level) {
        std::uint32_t face = 0;
        double s = 0.0;
        double t = 0.0;
        FaceFromDirection(direction, face, s, t);
        const double tiles = static_cast<double>(std::uint64_t{1} << level);
        const auto cell = [&](double value) {
            return static_cast<std::uint32_t>(std::clamp(std::floor((value + 1.0) * 0.5 * tiles), 0.0, tiles - 1.0));
        };
        return {static_cast<std::uint8_t>(face), static_cast<std::uint8_t>(level), cell(s), cell(t)};
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
}
