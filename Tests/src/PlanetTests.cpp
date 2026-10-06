// Tests des planètes en cube-sphère : projection et son inverse, tuiles voisines qui se raccordent (même dans un
// coin du cube), relief reproductible, sens des triangles et sphères englobantes.
#include "Check.hpp"

#include "Flux/Graphics/Planet.hpp"

#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <random>

using namespace Flux;

namespace {
    Math::Vector3d Position(const Planet::TileMesh& mesh, std::uint32_t index) {
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
        Planet::FaceFromDirection(direction, face, s, t);
        CHECK(face < 6 && std::abs(s) <= 1.0 + 1.0e-12 && std::abs(t) <= 1.0 + 1.0e-12);
        CHECK(Math::Length(Planet::DirectionFromFace(face, s, t) - direction) < 1.0e-12);
    }

    // Relief reproductible, borné, et différent d'une graine à l'autre.
    const Planet::Terrain terrain(60000.0, 2500.0, 3);
    const Planet::Terrain same(60000.0, 2500.0, 3);
    const Planet::Terrain other(60000.0, 2500.0, 4);
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
    const Planet::Terrain earth(6.0e6, 8000.0, 1);
    CHECK(earth.GetMaxLevel() == 18);
    CHECK(earth.GetOctaves(0) < earth.GetOctaves(10) && earth.GetOctaves(10) <= earth.GetOctaves(18));

    // Deux tuiles voisines d'une même face : bords confondus, normales égales (calculées avec une rangée de plus).
    Planet::TileMesh left;
    Planet::TileMesh right;
    Planet::BuildTile(terrain, {4, 3, 2, 5}, left);
    Planet::BuildTile(terrain, {4, 3, 3, 5}, right);
    CHECK(left.vertices.size() == Planet::TileVertexCount);
    for (std::uint32_t j = 0; j < Planet::TileSide; ++j) {
        const std::uint32_t a = j * Planet::TileSide + Planet::TileQuads;
        const std::uint32_t b = j * Planet::TileSide;
        CHECK(Math::Length(Position(left, a) - Position(right, b)) < 0.01);
        CHECK(Math::Length(left.vertices[a].normal - right.vertices[b].normal) < 1.0e-4f);
    }

    // Arête du cube : le bord droit de +Z (s = 1) est le bord gauche de +X (s = -1), mêmes t.
    Planet::TileMesh frontEdge;
    Planet::TileMesh sideEdge;
    Planet::BuildTile(terrain, {4, 2, 3, 1}, frontEdge);
    Planet::BuildTile(terrain, {0, 2, 0, 1}, sideEdge);
    for (std::uint32_t j = 0; j < Planet::TileSide; ++j) {
        const std::uint32_t a = j * Planet::TileSide + Planet::TileQuads;
        const std::uint32_t b = j * Planet::TileSide;
        CHECK(Math::Length(Position(frontEdge, a) - Position(sideEdge, b)) < 0.01);
        CHECK(Math::Dot(frontEdge.vertices[a].normal, sideEdge.vertices[b].normal) > 0.99f);
    }

    // Tous les sommets (jupes comprises) dans la sphère de la tuile, et dans celle estimée sans la générer.
    for (const Planet::TileKey key : {Planet::TileKey{0, 0, 0, 0}, Planet::TileKey{2, 4, 9, 3}, Planet::TileKey{5, 7, 100, 27}}) {
        Planet::TileMesh mesh;
        Planet::BuildTile(terrain, key, mesh);
        const Planet::TileBounds bounds = Planet::GetTileBounds(terrain, key);
        CHECK(bounds.edge > 0.0);
        for (std::uint32_t v = 0; v < Planet::TileVertexCount; ++v) {
            CHECK(Math::Length(Math::Vector3d(mesh.vertices[v].position)) <= mesh.radius + 1.0e-3);
            CHECK(Math::Length(Position(mesh, v) - bounds.center) <= bounds.radius * 1.05);
        }
    }

    // Indices : tous valides ; terrain vu de dehors dans le sens trigonométrique ; jupes tournées vers l'extérieur
    // de la tuile (sur une sphère lisse).
    const std::vector<std::uint32_t> indices = Planet::BuildTileIndices();
    constexpr std::uint32_t gridIndices = 6 * Planet::TileQuads * Planet::TileQuads;
    CHECK(indices.size() == gridIndices + 4 * 6 * Planet::TileQuads);
    const Planet::Terrain smooth(60000.0, 0.0, 1);
    Planet::TileMesh flat;
    Planet::BuildTile(smooth, {1, 3, 4, 6}, flat);
    for (std::size_t i = 0; i < indices.size(); i += 3) {
        CHECK(indices[i] < Planet::TileVertexCount && indices[i + 1] < Planet::TileVertexCount && indices[i + 2] < Planet::TileVertexCount);
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

    // Génération en tâches de fond : mêmes tuiles qu'en direct, toutes rendues, chacune avec sa demande.
    {
        const auto shared = std::make_shared<const Planet::Terrain>(60000.0, 800.0, 5);
        Planet::TileGenerator generator(2);
        CHECK(generator.GetThreadCount() == 2);
        std::vector<Planet::TileGenerator::Job> jobs;
        for (std::uint32_t i = 0; i < 8; ++i) {
            jobs.push_back({shared, 100 + i, Planet::TileKey{static_cast<std::uint8_t>(i % 6), 3, i, 7 - i}});
        }
        generator.Submit(jobs);
        CHECK(jobs.empty());
        std::vector<Planet::TileGenerator::Result> results;
        for (int wait = 0; wait < 1000 && results.size() < 8; ++wait) {
            generator.Collect(results, 3);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(results.size() == 8);
        for (const Planet::TileGenerator::Result& result : results) {
            const std::uint32_t i = static_cast<std::uint32_t>(result.owner - 100);
            CHECK(i < 8 && result.key == (Planet::TileKey{static_cast<std::uint8_t>(i % 6), 3, i, 7 - i}));
            Planet::TileMesh direct;
            Planet::BuildTile(*shared, result.key, direct);
            CHECK(result.mesh.center.x == direct.center.x && result.mesh.vertices.size() == direct.vertices.size());
            CHECK(result.mesh.vertices[500].position.y == direct.vertices[500].position.y);
        }
        // Les tâches pas commencées se reprennent ; le générateur s'arrête proprement avec du travail en file.
        for (std::uint32_t i = 0; i < 64; ++i) {
            jobs.push_back({shared, i, Planet::TileKey{0, 6, i, i}});
        }
        generator.Submit(jobs);
        std::vector<Planet::TileGenerator::Job> waiting;
        generator.TakeWaiting(waiting);
        CHECK(waiting.size() <= 64);
        generator.Submit(waiting);
    }
    return true;
}
