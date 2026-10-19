// Tuiles des planètes calculées par le GPU (shaders/PlanetTile.slang) comparées à la référence en double du CPU
// (Planetology::BuildTile) : mêmes sommets, normales et glissements, à quelques millimètres près. Ignorés (réussis) sur
// une machine sans GPU Vulkan 1.3 (intégration continue).
#include "Check.hpp"

#include "Flux/Planetology/Planet.hpp"
#include "Planetology/PlanetTileCompute.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Flux;

namespace {
    struct Case
    {
        const Planetology::Terrain* terrain;
        Planetology::TileKey key;
    };
}

bool RunPlanetGpuTests() {
    auto device = RHI::Device::CreateHeadless();
    if (!device) {
        std::printf("       GPU ignoré : %s\n", device.error().c_str());
        return true;
    }
    auto compute = Planetology::PlanetTileCompute::Create(**device, RHI::MemoryType::Readback);
    if (!compute) {
        std::printf("ÉCHEC %s\n", compute.error().c_str());
        return false;
    }

    // Une petite planète (celle des scènes de test), une grande aux montagnes de 20 km (couches décalées de 4 octaves)
    // et une de la taille de la Terre (décalées de 9) ; à chacune une racine, une tuile moyenne et une tuile du niveau
    // le plus fin, ailleurs sur une autre face.
    const Planetology::Terrain small(60000.0, 800.0, 5);
    const Planetology::Terrain large(200000.0, 4500.0, 7, 20000.0);
    const Planetology::Terrain earth(6.37e6, 8000.0, 42, 20000.0);
    CHECK(small.GetScaleShift() == 0 && large.GetScaleShift() == 4 && earth.GetScaleShift() == 9);
    std::vector<Case> cases;
    for (const Planetology::Terrain* terrain : {&small, &large, &earth}) {
        const std::uint32_t max = terrain->GetMaxLevel();
        const std::uint32_t middle = max / 2;
        cases.push_back({terrain, {0, 0, 0, 0}});
        cases.push_back({terrain, {3, static_cast<std::uint8_t>(middle), (1u << middle) / 3u, (1u << middle) / 2u}});
        cases.push_back({terrain, {4, static_cast<std::uint8_t>(max), (1u << max) / 3u, (1u << max) / 5u}});
        cases.push_back({terrain, {5, static_cast<std::uint8_t>(max), (1u << max) - 1u, 7u}});
        // Au plus haut d'un échantillon de directions : dans une chaîne de montagnes, où les sommets, les ravines et la
        // déformation comptent toutes.
        Math::Vector3d peak{0.0, 1.0, 0.0};
        double highest = -1.0e30;
        for (int k = 0; k < 2000; ++k) {
            const double z = 1.0 - 2.0 * (k + 0.5) / 2000.0;
            const double r = std::sqrt(1.0 - z * z);
            const Math::Vector3d direction{r * std::cos(2.39996 * k), z, r * std::sin(2.39996 * k)};
            if (const double height = terrain->GetHeight(direction, 8); height > highest) {
                highest = height;
                peak = direction;
            }
        }
        cases.push_back({terrain, Planetology::FindTile(peak, middle)});
        cases.push_back({terrain, Planetology::FindTile(peak, max)});
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

    std::vector<ModelVertex> vertices(Planetology::TileVertexCount);
    std::vector<Planetology::MorphVertex> morph(Planetology::TileVertexCount);
    for (std::size_t n = 0; n < cases.size(); ++n) {
        const Case& c = cases[n];
        Planetology::TileMesh mesh;
        Planetology::BuildTile(*c.terrain, c.key, mesh);
        (*compute)->ReadTile(tiles[n], vertices.data(), morph.data());
        // Erreur permise : le float de la position (sa taille), celui du relief, et le millimètre.
        const double tolerance = 0.005 + 1.0e-6 * mesh.radius + 4.0e-6 * c.terrain->GetRelief();
        double worst = 0.0;
        for (std::uint32_t v = 0; v < Planetology::TileVertexCount; ++v) {
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
