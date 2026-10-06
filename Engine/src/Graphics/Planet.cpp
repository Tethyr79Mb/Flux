#include "Flux/Graphics/Planet.hpp"

#include <algorithm>
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
        // Continents : bruit fractal (fBm). Le décalage sépare les deux bruits (sinon les crêtes suivraient les côtes).
        double base = 0.0;
        double amplitude = 1.0;
        Math::Vector3d p = direction * BaseFrequency;
        for (std::uint32_t k = 0; k < octaves; ++k) {
            base += amplitude * Noise(p);
            amplitude *= 0.5;
            p *= 2.0;
        }
        base /= 0.5 * AmplitudeSum; // environ -1 à 1
        // Montagnes : crêtes (1 - |bruit|)², seulement sur les terres hautes.
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
        return m_relief * std::clamp(0.6 * base + 0.6 * ridge * land, -1.0, 1.0);
    }

    double Terrain::GetOmittedRelief(std::uint32_t octaves) const {
        // Les octaves de rang >= octaves pèsent ensemble 2 x 0,5^octaves de la somme (base) ; les crêtes aussi.
        return m_relief * 1.2 * 2.0 * std::ldexp(1.0, -static_cast<int>(std::min(octaves, MaxRidgeOctaves)));
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

        // Jupes : le bord parcouru dans le sens trigonométrique (bas, droite, haut, gauche), chaque sommet descendu
        // vers le centre de la planète. Assez profondes pour couvrir l'écart avec une voisine plus grossière (le
        // relief qu'elle n'a pas) ; la normale reste celle du bord, la jupe se confond avec le terrain.
        const double edge = Math::Length(directions[Wide + 1 + TileQuads] - directions[Wide + 1]) * R;
        const double depth = 0.05 * edge + 2.0 * terrain.GetOmittedRelief(octaves) + 1.0;
        std::uint32_t next = TileSide * TileSide;
        const auto skirt = [&](std::uint32_t i, std::uint32_t j) {
            const std::uint32_t w = (j + 1) * Wide + (i + 1);
            const Math::Vector3d local = positions[w] - directions[w] * depth - mesh.center;
            mesh.vertices[next++] = {Math::Vector3f(local), vertex(i, j).normal, {0.0f, 0.0f}};
            mesh.radius = std::max(mesh.radius, Math::Length(local));
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
}
