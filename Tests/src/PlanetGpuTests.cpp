// Tuiles des planètes calculées par le GPU (shaders/PlanetTile.slang) comparées à la référence en double du CPU
// (Planetology::BuildTile) : mêmes sommets, normales et glissements, à quelques centimètres près. Ignorés (réussis) sur
// une machine sans GPU Vulkan 1.3, sauf avec FLUX_TESTS_GPU=1 (la CI, sur le GPU logiciel lavapipe).
#include "Check.hpp"

#include "Flux/Planetology/Planet.hpp"
#include "Planetology/PlanetRenderer.hpp"
#include "Planetology/PlanetTileCompute.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

using namespace Flux;

namespace {
    // Sol sous une caméra qui tourne sur place (PlanetRenderer) : un enfant pas encore calculé qui entre dans le champ
    // ne remplace que son quart de la parente, pas toute la parente. Elle revenait pour une image jusque sous la caméra :
    // le sol clignotait, plus grossier, penché et sans arbres, à chaque mouvement de caméra au début du jeu.
    bool CheckTurningCamera(RHI::Device& device) {
        auto created = Planetology::PlanetRenderer::Create(device);
        if (!created) {
            std::printf("ÉCHEC %s\n", created.error().c_str());
            return false;
        }
        Planetology::PlanetRenderer& renderer = **created;
        const PlanetComponent planet{.radius = 60000.0f, .relief = 800.0f, .seed = 5};
        const Planetology::Terrain terrain(planet.radius, planet.relief, planet.seed, planet.mountainWidth);
        // Pas au centre de la face : c'est le coin commun de quatre tuiles à tous les niveaux.
        const Math::Vector3d up = Math::Normalize(Math::Vector3d{0.13, 1.0, 0.07});
        const double ground = planet.radius + terrain.GetHeight(up, terrain.GetOctaves(terrain.GetMaxLevel()));
        const Math::Vector3d position = up * -(ground + 2.0); // centre de la planète, relatif à la caméra
        const auto frame = [&](const Math::Vector3d& forward, std::vector<Planetology::PlanetRenderer::Tile>& tiles) {
            // Champ de 100° autour de `forward`.
            const auto inView = [&forward](const Math::Vector3f& center, float radius) {
                const double distance = Math::Length(Math::Vector3d(center));
                if (distance <= radius) {
                    return true;
                }
                const double angle = std::acos(std::clamp(Math::Dot(Math::Vector3d(center), forward) / distance, -1.0, 1.0));
                return angle - std::asin(std::min(static_cast<double>(radius) / distance, 1.0)) < 50.0 * std::numbers::pi / 180.0;
            };
            tiles.clear();
            renderer.BeginFrame(0);
            renderer.Collect(static_cast<Entity>(1), planet, position, Math::Quaternionf{}, inView, tiles);
            renderer.EndFrame(0);
            return device.Submit([&](RHI::CommandList& commands) { renderer.Compute(commands, 0); });
        };
        // Tuile dessinée sous la caméra (entière ou quart), et la parente d'un quart dessiné qui la couvre.
        const auto under = [&up](const std::vector<Planetology::PlanetRenderer::Tile>& tiles) {
            int level = -1;
            bool quarterAbove = false;
            for (const Planetology::PlanetRenderer::Tile& tile : tiles) {
                if (Planetology::FindTile(up, tile.key.level) == tile.key) {
                    level = std::max(level, static_cast<int>(tile.key.level));
                }
                const Planetology::TileKey parent{tile.key.face, static_cast<std::uint8_t>(tile.key.level - 1u), tile.key.x / 2u, tile.key.y / 2u};
                if (tile.indexCount == Planetology::TileQuarterIndices && Planetology::FindTile(up, parent.level) == parent) {
                    quarterAbove = true;
                }
            }
            return std::pair{level, quarterAbove};
        };

        std::vector<Planetology::PlanetRenderer::Tile> tiles;
        const Math::Vector3d east{1.0, 0.0, 0.0};
        for (int n = 0; n < 300; ++n) {
            CHECK(frame(east, tiles));
            if (n > 2 && renderer.GetPendingTileCount() == 0) {
                break;
            }
        }
        CHECK(renderer.GetPendingTileCount() == 0);
        const int settled = under(tiles).first;
        CHECK(settled >= 10);
        // Un quart de tour : le sol au loin n'est pas encore calculé, celui sous la caméra l'est.
        CHECK(frame({0.0, 0.0, 1.0}, tiles));
        const auto [turned, quarterAbove] = under(tiles);
        CHECK(quarterAbove); // une parente de la tuile sous la caméra attend un enfant : le cas qui clignotait
        if (turned != settled) {
            std::printf("ÉCHEC sol sous la caméra au niveau %d après un quart de tour (%d avant)\n", turned, settled);
            return false;
        }
        return true;
    }

    struct Case
    {
        const Planetology::Terrain* terrain;
        Planetology::TileKey key;
    };
}

namespace {
// Les boîtes des paquets des mesh shaders (PlanetMesh.slang) contiennent chacun de leurs sommets, dans les deux formes
// du glissement, jupes comprises : sinon un morceau de sol visible serait écarté.
bool CheckMeshlets(const Planetology::PlanetTileCompute& compute, std::uint32_t tile, const std::vector<ModelVertex>& vertices,
                   const std::vector<Planetology::MorphVertex>& morph) {
    using namespace Planetology;
    std::array<PlanetTileCompute::Meshlet, TileMeshletCount> meshlets{};
    compute.ReadMeshlets(tile, meshlets.data());
    const auto inside = [&](const PlanetTileCompute::Meshlet& box, std::uint32_t v) {
        for (const Math::Vector3f p : {vertices[v].position, vertices[v].position + morph[v].offset}) {
            for (int axis = 0; axis < 3; ++axis) {
                const float value = axis == 0 ? p.x : axis == 1 ? p.y : p.z;
                const float margin = 1.0e-4f * (1.0f + std::abs(value));
                if (value < box.low[axis] - margin || value > box.high[axis] + margin) {
                    std::printf("ÉCHEC sommet %u hors de la boîte de son paquet\n", v);
                    return false;
                }
            }
        }
        return true;
    };
    for (std::uint32_t my = 0; my < TileMeshletSide; ++my) {
        for (std::uint32_t mx = 0; mx < TileMeshletSide; ++mx) {
            const PlanetTileCompute::Meshlet& box = meshlets[my * TileMeshletSide + mx];
            for (std::uint32_t j = my * TileMeshletQuads; j <= (my + 1) * TileMeshletQuads; ++j) {
                for (std::uint32_t i = mx * TileMeshletQuads; i <= (mx + 1) * TileMeshletQuads; ++i) {
                    if (!inside(box, j * TileSide + i)) {
                        return false;
                    }
                    // Jupes, dans l'ordre de BuildTile : bas, droite, haut, gauche.
                    const std::uint32_t skirts = TileSide * TileSide;
                    if ((j == 0 && !inside(box, skirts + i)) || (i == TileQuads && !inside(box, skirts + TileSide + j))
                        || (j == TileQuads && !inside(box, skirts + 2 * TileSide + (TileQuads - i)))
                        || (i == 0 && !inside(box, skirts + 3 * TileSide + (TileQuads - j)))) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}
}

bool RunPlanetGpuTests() {
    auto device = RHI::Device::CreateHeadless();
    if (!device) {
        return GpuMissing("GPU", device.error().c_str());
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
        CHECK(CheckMeshlets(**compute, tiles[n], vertices, morph));
    }
    return CheckTurningCamera(**device);
}
