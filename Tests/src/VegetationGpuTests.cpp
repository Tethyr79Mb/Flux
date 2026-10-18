// Arbres semés par le GPU (computeTrees dans shaders/PlanetTile.slang) : posés sur le relief de la référence du CPU
// (Terrain::GetHeight au détail le plus fin), droits, aux mêmes places d'une fois à l'autre, et absents là où la pente
// les refuse. Ignorés (réussis) sur une machine sans GPU Vulkan 1.3 (intégration continue).
#include "Check.hpp"

#include "Flux/Planetology/Planet.hpp"
#include "Planetology/VegetationRenderer.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace Flux;
using Planetology::VegetationRenderer;

namespace {
    // Sème les plaines `keys` (caméra au centre de la planète) et relit leurs arbres.
    bool Sow(RHI::Device& device, const Planetology::Terrain& terrain, const PlanetComponent& planet,
             const VegetationComponent& vegetation, std::span<const Planetology::TileKey> keys,
             std::vector<std::vector<VegetationRenderer::Tree>>& trees) {
        auto renderer = VegetationRenderer::Create(device, RHI::MemoryType::Readback);
        if (!renderer) {
            std::printf("ÉCHEC %s\n", renderer.error().c_str());
            return false;
        }
        std::vector<Planetology::PlanetRenderer::Plain> plains;
        for (const Planetology::TileKey key : keys) {
            plains.push_back({key, Planetology::GetTileBounds(terrain, key).origin, 0.0});
        }
        (*renderer)->BeginFrame(0);
        (*renderer)->SetPlanet(static_cast<Entity>(1), terrain, planet, vegetation, Math::Quaternionf{}, plains);
        (*renderer)->EndFrame(0, {{0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 1.0f, 1.0f}, {}, {});
        CHECK(device.Submit([&](RHI::CommandList& commands) { (*renderer)->Compute(commands, 0); }));
        trees.assign(keys.size(), {});
        for (std::size_t k = 0; k < keys.size(); ++k) {
            CHECK((*renderer)->ReadTrees(keys[k], trees[k]));
        }
        return true;
    }
}

bool RunVegetationGpuTests() {
    auto device = RHI::Device::CreateHeadless();
    if (!device) {
        std::printf("       GPU ignoré : %s\n", device.error().c_str());
        return true;
    }

    // Partout chaud et humide, ni mer ni neige à portée : seules la densité, les forêts et la pente décident.
    PlanetComponent planet;
    planet.radius = 60000.0f;
    planet.relief = 800.0f;
    planet.seed = 5;
    planet.temperature = 25.0f;
    planet.poleTemperature = 25.0f;
    planet.humidity = 1.0f;
    planet.seaLevel = -200.0f;
    planet.snowLevel = 1000.0f;
    VegetationComponent vegetation;
    vegetation.density = 1.0f;
    vegetation.cover = 1.0f;
    vegetation.maxSlope = 90.0f;
    const Planetology::Terrain terrain(planet.radius, planet.relief, planet.seed);
    const std::uint32_t level = VegetationRenderer::GetPlainLevel(terrain);
    CHECK(level > 0 && level < terrain.GetMaxLevel());
    const std::vector<Planetology::TileKey> keys{Planetology::FindTile(Math::Normalize(Math::Vector3d{0.3, 0.8, -0.5}), level),
                                                 Planetology::FindTile(Math::Normalize(Math::Vector3d{-0.9, -0.2, 0.4}), level)};

    std::vector<std::vector<VegetationRenderer::Tree>> first;
    CHECK(Sow(**device, terrain, planet, vegetation, keys, first));
    const std::uint32_t octaves = terrain.GetOctaves(terrain.GetMaxLevel());
    for (std::size_t k = 0; k < keys.size(); ++k) {
        // Forêt partout et densité 1 : presque toutes les places.
        CHECK(first[k].size() > VegetationRenderer::TreesPerPlain * 3 / 4);
        const Math::Vector3d origin = Planetology::GetTileBounds(terrain, keys[k]).origin;
        double worst = 0.0;
        for (const VegetationRenderer::Tree& tree : first[k]) {
            const Math::Vector3d local = origin + Math::Vector3d(tree.position);
            const Math::Vector3d direction = Math::Normalize(local);
            const double error = std::abs(Math::Length(local) - terrain.GetRadius() - terrain.GetHeight(direction, octaves));
            worst = std::max(worst, error);
            CHECK(Math::Dot(Math::Vector3d(tree.up), direction) > 0.99999);
            CHECK(tree.scale >= vegetation.minScale && tree.scale <= vegetation.maxScale);
            CHECK(tree.angle >= 0.0f && tree.angle < 6.3f);
        }
        // Le float de la position (à quelques centaines de mètres du point de référence) et celui du relief.
        if (worst > 0.01) {
            std::printf("ÉCHEC plaine %zu : arbre à %.3f m du sol\n", k, worst);
            return false;
        }
    }

    // Mêmes places et mêmes arbres une seconde fois (l'ordre, lui, dépend du GPU).
    std::vector<std::vector<VegetationRenderer::Tree>> second;
    CHECK(Sow(**device, terrain, planet, vegetation, keys, second));
    const auto byPlace = [](const VegetationRenderer::Tree& a, const VegetationRenderer::Tree& b) {
        return a.position.x != b.position.x ? a.position.x < b.position.x : a.position.z < b.position.z;
    };
    for (std::size_t k = 0; k < keys.size(); ++k) {
        CHECK(second[k].size() == first[k].size());
        std::ranges::sort(first[k], byPlace);
        std::ranges::sort(second[k], byPlace);
        for (std::size_t t = 0; t < first[k].size(); ++t) {
            CHECK(first[k][t].position == second[k][t].position && first[k][t].scale == second[k][t].scale
                  && first[k][t].angle == second[k][t].angle);
        }
    }

    // Aucune pente permise : plus aucun arbre ; forêts à moitié : moins d'arbres.
    vegetation.maxSlope = 0.0f;
    CHECK(Sow(**device, terrain, planet, vegetation, keys, second));
    CHECK(second[0].empty() && second[1].empty());
    vegetation.maxSlope = 90.0f;
    vegetation.cover = 0.5f;
    CHECK(Sow(**device, terrain, planet, vegetation, keys, second));
    CHECK(second[0].size() + second[1].size() < first[0].size() + first[1].size());
    return true;
}
