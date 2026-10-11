// Tuiles des planètes calculées par le GPU (shaders/PlanetTile.slang) comparées à la référence en double du CPU
// (Planet::BuildTile) : mêmes sommets, normales et glissements, à quelques millimètres près. Ignorés (réussis) sur
// une machine sans GPU Vulkan 1.3 (intégration continue).
#include "Check.hpp"

#include "Flux/Graphics/Planet.hpp"
#include "Renderer/PlanetTileCompute.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Flux;

namespace {
    struct Case
    {
        const Planet::Terrain* terrain;
        Planet::TileKey key;
    };
}

bool RunPlanetGpuTests() {
    auto device = RHI::Device::CreateHeadless();
    if (!device) {
        std::printf("       GPU ignoré : %s\n", device.error().c_str());
        return true;
    }
    auto compute = PlanetTileCompute::Create(**device, RHI::MemoryType::Readback);
    if (!compute) {
        std::printf("ÉCHEC %s\n", compute.error().c_str());
        return false;
    }

    // Une petite planète (celle des scènes de test) et une de la taille de la Terre ; à chacune une racine, une tuile
    // moyenne et une tuile du niveau le plus fin, ailleurs sur une autre face.
    const Planet::Terrain small(60000.0, 800.0, 5);
    const Planet::Terrain earth(6.37e6, 8000.0, 42);
    std::vector<Case> cases;
    for (const Planet::Terrain* terrain : {&small, &earth}) {
        const std::uint32_t max = terrain->GetMaxLevel();
        const std::uint32_t middle = max / 2;
        cases.push_back({terrain, {0, 0, 0, 0}});
        cases.push_back({terrain, {3, static_cast<std::uint8_t>(middle), (1u << middle) / 3u, (1u << middle) / 2u}});
        cases.push_back({terrain, {4, static_cast<std::uint8_t>(max), (1u << max) / 3u, (1u << max) / 5u}});
        cases.push_back({terrain, {5, static_cast<std::uint8_t>(max), (1u << max) - 1u, 7u}});
    }

    // Toutes dans la même page : chacune doit écrire dans sa case.
    std::vector<std::uint32_t> tiles;
    for (const Case& c : cases) {
        auto tile = (*compute)->CreateTile();
        CHECK(tile.has_value());
        CHECK((*compute)->Add(0, *c.terrain, c.key, *tile));
        tiles.push_back(*tile);
    }
    CHECK((*device)->Submit([&](RHI::CommandList& commands) { (*compute)->Record(commands, 0); }));
    const auto stats = (*compute)->GetStats(0);
    CHECK(stats.size() == cases.size());

    std::vector<ModelVertex> vertices(Planet::TileVertexCount);
    std::vector<Planet::MorphVertex> morph(Planet::TileVertexCount);
    for (std::size_t n = 0; n < cases.size(); ++n) {
        const Case& c = cases[n];
        Planet::TileMesh mesh;
        Planet::BuildTile(*c.terrain, c.key, mesh);
        (*compute)->ReadTile(tiles[n], vertices.data(), morph.data());
        // Erreur permise : le float de la position (sa taille), celui du relief, et le millimètre.
        const double tolerance = 0.005 + 1.0e-6 * mesh.radius + 4.0e-6 * c.terrain->GetRelief();
        double worst = 0.0;
        for (std::uint32_t v = 0; v < Planet::TileVertexCount; ++v) {
            const double error = Math::Length(Math::Vector3d(vertices[v].position) - Math::Vector3d(mesh.vertices[v].position));
            const double morphError = Math::Length(Math::Vector3d(morph[v].offset) - Math::Vector3d(mesh.morph[v].offset));
            worst = std::max({worst, error, morphError});
            if (error > tolerance || morphError > tolerance) {
                std::printf("ÉCHEC tuile %zu (niveau %u), sommet %u : écart %.4f m, glissement %.4f m (permis %.4f m)\n", n,
                            static_cast<unsigned>(c.key.level), v, error, morphError, tolerance);
                return false;
            }
            CHECK(Math::Dot(vertices[v].normal, mesh.vertices[v].normal) > 0.999f);
            CHECK(Math::Dot(morph[v].normal, mesh.morph[v].normal) > 0.999f);
        }
        CHECK(std::abs(stats[n].lowest - mesh.lowest) < 2.0 * tolerance + 1.0e-6 * mesh.lowest);
        CHECK(std::abs(stats[n].radius - mesh.radius) < 2.0 * tolerance);
    }
    return true;
}
