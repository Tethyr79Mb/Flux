// Tests des planètes en cube-sphère : projection et son inverse, tuiles voisines qui se raccordent (même dans un
// coin du cube), relief reproductible, sens des triangles et sphères englobantes.
#include "Check.hpp"

#include "Flux/Planetology/Atmosphere.hpp"
#include "Flux/Planetology/Planet.hpp"
#include "Flux/Scene/Components.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <random>

using namespace Flux;

namespace {
    Math::Vector3d Position(const Planetology::TileMesh& mesh, std::uint32_t index) {
        return mesh.center + Math::Vector3d(mesh.vertices[index].position);
    }
}

bool RunPlanetTests() {
    // Direction -> (face, s, t) -> direction.
    std::mt19937 random(7);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    for (int i = 0; i < 1000; ++i) {
        const Math::Vector3d direction = Math::Normalize(Math::Vector3d{unit(random), unit(random), unit(random)});
        std::uint32_t face = 0;
        double s = 0.0;
        double t = 0.0;
        Planetology::FaceFromDirection(direction, face, s, t);
        CHECK(face < 6 && std::abs(s) <= 1.0 + 1.0e-12 && std::abs(t) <= 1.0 + 1.0e-12);
        CHECK(Math::Length(Planetology::DirectionFromFace(face, s, t) - direction) < 1.0e-12);
    }

    // Relief reproductible, borné, et différent d'une graine à l'autre.
    const Planetology::Terrain terrain(60000.0, 2500.0, 3);
    const Planetology::Terrain same(60000.0, 2500.0, 3);
    const Planetology::Terrain other(60000.0, 2500.0, 4);
    bool differs = false;
    for (int i = 0; i < 200; ++i) {
        const Math::Vector3d direction = Math::Normalize(Math::Vector3d{unit(random), unit(random), unit(random)});
        const double height = terrain.GetHeight(direction, 12);
        CHECK(height == same.GetHeight(direction, 12));
        CHECK(std::abs(height) <= terrain.GetRelief());
        differs = differs || std::abs(height - other.GetHeight(direction, 12)) > 1.0;
    }
    CHECK(differs);
    // Environ 1,5 m entre deux sommets au niveau le plus fin, et des octaves de plus en plus nombreuses.
    const Planetology::Terrain earth(6.0e6, 8000.0, 1);
    CHECK(earth.GetMaxLevel() == 18);
    CHECK(earth.GetOctaves(0) < earth.GetOctaves(10) && earth.GetOctaves(10) <= earth.GetOctaves(18));

    // Deux tuiles voisines d'une même face : bords confondus, normales égales (calculées avec une rangée de plus).
    Planetology::TileMesh left;
    Planetology::TileMesh right;
    Planetology::BuildTile(terrain, {4, 3, 2, 5}, left);
    Planetology::BuildTile(terrain, {4, 3, 3, 5}, right);
    CHECK(left.vertices.size() == Planetology::TileVertexCount);
    for (std::uint32_t j = 0; j < Planetology::TileSide; ++j) {
        const std::uint32_t a = j * Planetology::TileSide + Planetology::TileQuads;
        const std::uint32_t b = j * Planetology::TileSide;
        CHECK(Math::Length(Position(left, a) - Position(right, b)) < 0.01);
        CHECK(Math::Length(left.vertices[a].normal - right.vertices[b].normal) < 1.0e-4f);
    }

    // Arête du cube : le bord droit de +Z (s = 1) est le bord gauche de +X (s = -1), mêmes t.
    Planetology::TileMesh frontEdge;
    Planetology::TileMesh sideEdge;
    Planetology::BuildTile(terrain, {4, 2, 3, 1}, frontEdge);
    Planetology::BuildTile(terrain, {0, 2, 0, 1}, sideEdge);
    for (std::uint32_t j = 0; j < Planetology::TileSide; ++j) {
        const std::uint32_t a = j * Planetology::TileSide + Planetology::TileQuads;
        const std::uint32_t b = j * Planetology::TileSide;
        CHECK(Math::Length(Position(frontEdge, a) - Position(sideEdge, b)) < 0.01);
        CHECK(Math::Dot(frontEdge.vertices[a].normal, sideEdge.vertices[b].normal) > 0.99f);
    }

    // Tous les sommets (jupes comprises) dans la sphère de la tuile, et dans celle estimée sans la générer.
    for (const Planetology::TileKey key : {Planetology::TileKey{0, 0, 0, 0}, Planetology::TileKey{2, 4, 9, 3}, Planetology::TileKey{5, 7, 100, 27}}) {
        Planetology::TileMesh mesh;
        Planetology::BuildTile(terrain, key, mesh);
        const Planetology::TileBounds bounds = Planetology::GetTileBounds(terrain, key);
        CHECK(bounds.edge > 0.0);
        CHECK(Math::Length(bounds.origin - mesh.center) < 1.0e-6 * terrain.GetRadius());
        CHECK(Planetology::TileKey::Unpack(key.Pack()) == key);
        for (std::uint32_t v = 0; v < Planetology::TileVertexCount; ++v) {
            CHECK(Math::Length(Math::Vector3d(mesh.vertices[v].position)) <= mesh.radius + 1.0e-3);
            CHECK(Math::Length(Position(mesh, v) - bounds.center) <= bounds.radius * 1.05);
        }
    }

    // Indices : tous valides ; terrain vu de dehors dans le sens trigonométrique ; jupes tournées vers l'extérieur
    // de la tuile (sur une sphère lisse).
    const std::vector<std::uint32_t> indices = Planetology::BuildTileIndices();
    constexpr std::uint32_t gridIndices = 6 * Planetology::TileQuads * Planetology::TileQuads;
    CHECK(indices.size() == gridIndices + 4 * 6 * Planetology::TileQuads);
    const Planetology::Terrain smooth(60000.0, 0.0, 1);
    Planetology::TileMesh flat;
    Planetology::BuildTile(smooth, {1, 3, 4, 6}, flat);
    for (std::size_t i = 0; i < indices.size(); i += 3) {
        CHECK(indices[i] < Planetology::TileVertexCount && indices[i + 1] < Planetology::TileVertexCount && indices[i + 2] < Planetology::TileVertexCount);
        const Math::Vector3d a = Position(flat, indices[i]);
        const Math::Vector3d b = Position(flat, indices[i + 1]);
        const Math::Vector3d c = Position(flat, indices[i + 2]);
        const Math::Vector3d normal = Math::Cross(b - a, c - a);
        const Math::Vector3d middle = (a + b + c) / 3.0;
        if (i < gridIndices) {
            CHECK(Math::Dot(normal, middle) > 0.0);
        } else {
            const Math::Vector3d outward = middle - flat.center;
            CHECK(Math::Dot(normal, outward - Math::Normalize(middle) * Math::Dot(outward, Math::Normalize(middle))) > 0.0);
        }
    }

    // Forme du terrain : des plaines larges où l'on peut se poser (pas de bruit partout), des montagnes rares et
    // hautes, et les creux sous le niveau zéro. Sur une petite lune, et sur une planète de 200 km aux montagnes de 50 km
    // où les massifs prennent plus de place, mais où les basses terres restent plates (le détail qui suivait la
    // largeur des montagnes n'y laissait aucune plaine).
    for (const auto& [radius, relief, width] : {std::array{60000.0, 800.0, 0.0}, std::array{200000.0, 3000.0, 50000.0}}) {
        const Planetology::Terrain world(radius, relief, 1, width);
        const std::uint32_t octaves = world.GetOctaves(world.GetMaxLevel());
        // Pentes au pas d'un sommet (1,5 m) autour d'un point, en degrés.
        const auto medianSlope = [&](const Math::Vector3d& center) {
            const Math::Vector3d up = Math::Normalize(center);
            const Math::Vector3d east = Math::Normalize(Math::Cross(up, Math::Vector3d{0.0, 1.0, 0.0}));
            const Math::Vector3d north = Math::Cross(east, up);
            constexpr int N = 24;
            constexpr double step = 1.5;
            std::vector<double> heights(N * N);
            for (int y = 0; y < N; ++y) {
                for (int x = 0; x < N; ++x) {
                    const Math::Vector3d d = Math::Normalize(up * world.GetRadius() + east * (x * step) + north * (y * step));
                    heights[y * N + x] = world.GetHeight(d, octaves);
                }
            }
            std::vector<double> slopes;
            for (int y = 0; y + 1 < N; ++y) {
                for (int x = 0; x + 1 < N; ++x) {
                    const double dx = (heights[y * N + x + 1] - heights[y * N + x]) / step;
                    const double dy = (heights[(y + 1) * N + x] - heights[y * N + x]) / step;
                    slopes.push_back(std::atan(std::hypot(dx, dy)) * 180.0 / std::numbers::pi);
                }
            }
            std::ranges::nth_element(slopes, slopes.begin() + static_cast<std::ptrdiff_t>(slopes.size() / 2));
            return slopes[slopes.size() / 2];
        };
        // Répartition des hauteurs sur toute la sphère.
        int low = 0;
        int high = 0;
        int plains = 0;
        int land = 0;
        int lowland = 0; // terres sous 15 % du relief
        int flatLowland = 0;
        std::uniform_real_distribution<double> any(-1.0, 1.0);
        for (int i = 0; i < 300; ++i) {
            const Math::Vector3d d = Math::Normalize(Math::Vector3d{any(random), any(random), any(random)});
            const double h = world.GetHeight(d, octaves);
            if (h < -0.125 * relief) {
                ++low;
            }
            if (h > 0.5 * relief) {
                ++high;
            }
            if (h > 0.0) {
                ++land;
                // Une terre sur deux au moins est une plaine (moins de 5 degrés au pas d'un sommet) : on s'y pose.
                const bool flat = medianSlope(d * (world.GetRadius() + h)) < 5.0;
                plains += flat ? 1 : 0;
                if (h < 0.15 * relief) {
                    ++lowland;
                    flatLowland += flat ? 1 : 0;
                }
            }
        }
        CHECK(low > 30 && high >= 1);    // des creux, et quelques sommets
        CHECK(high < 60);                // les montagnes restent rares
        CHECK(land > 60 && flatLowland * 2 > lowland);
        CHECK(width > 0.0 || plains * 2 > land);
    }

    // Glissement vers la parente : chaque sommet arrive sur la forme de sa parente (sur un de ses sommets, ou au
    // milieu d'un bord de ses triangles), avec ses normales ; rien au niveau 0 ; deux voisines arrivent au même endroit.
    {
        Planetology::TileMesh root;
        Planetology::BuildTile(terrain, {3, 0, 0, 0}, root);
        CHECK(root.morph.size() == Planetology::TileVertexCount);
        CHECK(Math::Length(root.morph[100].offset) == 0.0f);
        const Planetology::TileKey parentKey{2, 4, 9, 3};
        Planetology::TileMesh parentMesh;
        Planetology::BuildTile(terrain, parentKey, parentMesh);
        const auto parentAt = [&](std::uint32_t i, std::uint32_t j) { return Position(parentMesh, j * Planetology::TileSide + i); };
        for (std::uint32_t c = 0; c < 4; ++c) {
            Planetology::TileMesh child;
            Planetology::BuildTile(terrain, parentKey.Child(c), child);
            const std::uint32_t pi0 = (c & 1u) * Planetology::TileQuads / 2;
            const std::uint32_t pj0 = (c >> 1) * Planetology::TileQuads / 2;
            for (std::uint32_t j = 0; j < Planetology::TileSide; ++j) {
                for (std::uint32_t i = 0; i < Planetology::TileSide; ++i) {
                    const std::uint32_t v = j * Planetology::TileSide + i;
                    const Math::Vector3d target = Position(child, v) + Math::Vector3d(child.morph[v].offset);
                    // Les deux sommets de la parente dont il est le milieu (le même s'il est à elle).
                    const std::uint32_t i0 = pi0 + i / 2;
                    const std::uint32_t j0 = pj0 + j / 2;
                    const Math::Vector3d expected = (parentAt(i0, j0) + parentAt(i0 + (i & 1u), j0 + (j & 1u))) * 0.5;
                    CHECK(Math::Length(target - expected) < 0.01);
                    if ((i & 1u) == 0 && (j & 1u) == 0) {
                        CHECK(Math::Dot(child.morph[v].normal, parentMesh.vertices[j0 * Planetology::TileSide + i0].normal) > 0.9999f);
                    }
                }
            }
        }
        for (std::uint32_t j = 0; j < Planetology::TileSide; ++j) {
            const std::uint32_t a = j * Planetology::TileSide + Planetology::TileQuads;
            const std::uint32_t b = j * Planetology::TileSide;
            const Math::Vector3d fromLeft = Position(left, a) + Math::Vector3d(left.morph[a].offset);
            const Math::Vector3d fromRight = Position(right, b) + Math::Vector3d(right.morph[b].offset);
            CHECK(Math::Length(fromLeft - fromRight) < 0.01);
        }
        // Distances de coupe : la moitié à chaque niveau.
        CHECK(std::abs(Planetology::GetSplitDistance(terrain, 3) - 2.0 * Planetology::GetSplitDistance(terrain, 4)) < 1.0e-6);
    }

    // Surface pour la collision : les mêmes sommets que la tuile affichée, au bit près ; la tuile d'une direction est
    // celle qui la contient.
    {
        const Planetology::Terrain surfaceTerrain(60000.0, 800.0, 2);
        const std::uint32_t level = surfaceTerrain.GetMaxLevel();
        const Planetology::TileKey key{4, static_cast<std::uint8_t>(level), (1u << level) / 3u, (1u << level) / 5u};
        Planetology::TileMesh mesh;
        Planetology::BuildTile(surfaceTerrain, key, mesh);
        Math::Vector3d center;
        std::vector<Math::Vector3f> surface;
        Planetology::BuildTileSurface(surfaceTerrain, key, center, surface);
        CHECK(center.x == mesh.center.x && center.y == mesh.center.y && center.z == mesh.center.z);
        CHECK(surface.size() == Planetology::TileSide * Planetology::TileSide);
        bool identical = true;
        for (std::uint32_t v = 0; v < surface.size(); ++v) {
            identical = identical && surface[v] == mesh.vertices[v].position;
        }
        CHECK(identical);
        // Le milieu de chaque quart de la tuile, et un point près d'un coin.
        for (const std::uint32_t v : {8u * Planetology::TileSide + 8u, 24u * Planetology::TileSide + 24u, Planetology::TileSide + 1u}) {
            CHECK(Planetology::FindTile(Math::Normalize(center + Math::Vector3d(surface[v])), key.level) == key);
        }
        CHECK(Planetology::FindTile({0.0, 0.0, 1.0}, 0) == (Planetology::TileKey{4, 0, 0, 0}));
    }

    // Atmosphère : ciel bleu à midi, soleil rouge au couchant, nuit de l'autre côté, rien vu de l'espace en regardant
    // ailleurs, et pas d'air du tout avec une densité nulle.
    {
        PlanetComponent planet;
        const Planetology::AtmosphereParameters air = Planetology::MakeAtmosphereParameters(planet);
        CHECK(air.top > air.radius && air.rayleigh.z > air.rayleigh.x);
        const Math::Vector3f sun{1.0f, 1.0f, 1.0f};
        const Math::Vector3f ground{0.0f, air.radius + 2.0f, 0.0f};
        const Math::Vector3f up{0.0f, 1.0f, 0.0f};
        Math::Vector3f transmittance;
        const Math::Vector3f zenith = Scatter(air, ground, up, 1.0e30f, up, sun, Planetology::AtmosphereSkySteps, transmittance);
        CHECK(zenith.z > zenith.y && zenith.y > zenith.x && zenith.x > 0.0f);
        CHECK(transmittance.x > 0.9f && transmittance.z < transmittance.x);
        const Math::Vector3f low = Math::Normalize(Math::Vector3f{1.0f, 0.02f, 0.0f});
        const Math::Vector3f setting = SunTransmittance(air, ground, low);
        CHECK(setting.x > 0.1f && setting.x > 2.0f * setting.z);
        CHECK(SunTransmittance(air, ground, up * -1.0f) == Math::Vector3f{});
        const Math::Vector3f space{0.0f, air.top * 3.0f, 0.0f};
        CHECK(Scatter(air, space, up, 1.0e30f, up, sun, Planetology::AtmosphereSkySteps, transmittance) == Math::Vector3f{});
        CHECK(transmittance == (Math::Vector3f{1.0f, 1.0f, 1.0f}));
        // Vue de l'espace vers le bord éclairé de la planète : le halo.
        const Math::Vector3f limb = Math::Normalize(Math::Vector3f{0.0f, -air.top * 3.0f, air.radius + 1000.0f});
        CHECK(Scatter(air, space, limb, 1.0e30f, Math::Vector3f{0.0f, 0.0f, 1.0f}, sun, Planetology::AtmosphereSkySteps, transmittance).z > 0.01f);
        // Lumière du ciel : nulle en pleine nuit, plus forte le jour qu'au couchant.
        const auto ambient = Planetology::ComputeAtmosphereAmbient(air, sun);
        CHECK(Math::Length(ambient.front()) < 1.0e-4f);
        CHECK(ambient.back().z > ambient[Planetology::AtmosphereAmbientSize / 2].z && ambient.back().z > 0.05f);
        planet.atmosphere = 0.0f;
        CHECK(Planetology::MakeAtmosphereParameters(planet).top == 0.0f);
    }
    return true;
}
