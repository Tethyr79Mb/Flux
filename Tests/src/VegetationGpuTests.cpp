// Arbres semés par le GPU (computeTrees dans shaders/PlanetTile.slang) : posés sur le relief de la référence du CPU
// (Terrain::GetHeight au détail le plus fin), droits, aux mêmes places d'une fois à l'autre, absents là où la pente
// les refuse, chacun d'une espèce de son biome (zones du relief, parts des espèces). Ignorés (réussis) sans GPU
// Vulkan 1.3, sauf avec FLUX_TESTS_GPU=1 (la CI, sur lavapipe).
#include "Check.hpp"

#include "Flux/Planetology/Planet.hpp"
#include "Planetology/VegetationRenderer.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <utility>
#include <memory>
#include <vector>

using namespace Flux;
using Planetology::VegetationRenderer;

namespace {
    using Planetology::VegetationSet;

    // Une espèce de tailles [minScale, maxScale].
    VegetationSet::Species Species(float minScale, float maxScale, float maxSlope) {
        return {{"a.glb", "a.glb", "a.glb"}, minScale, maxScale, maxSlope, 0.0f};
    }

    // Un biome partout (zones, climat et altitude sans limite), de couverture et densité données ; ses espèces (une
    // variante chacune) et leurs parts.
    void AddBiome(VegetationSet& set, std::array<float, 3> zones, float cover, float density,
                  std::initializer_list<std::pair<std::uint32_t, float>> species) {
        std::array<std::uint32_t, 4> entries{};
        std::array<float, 4> shares{1.0f, 1.0f, 1.0f, 1.0f};
        float total = 0.0f;
        for (const auto& [index, share] : species) {
            total += share;
        }
        float sum = 0.0f;
        std::uint32_t used = 0;
        for (const auto& [index, share] : species) {
            sum += share;
            entries[used] = index | (1u << 8);
            shares[used] = sum / total;
            ++used;
        }
        for (std::uint32_t s = used; s < 4; ++s) {
            entries[s] = entries[used - 1];
            shares[s] = 1.0f;
        }
        set.biomes.push_back({{zones[0], zones[1], zones[2], cover}, {-10.0f, 10.0f, -1000.0f, 1000.0f}, {-10.0f, 10.0f, density, 1.0f}, {}});
        set.biomeSpecies.push_back(entries);
        set.biomeShares.push_back(shares);
        ++set.revision;
    }

    std::uint32_t SpeciesOf(const VegetationRenderer::Tree& tree) {
        return static_cast<std::uint32_t>(tree.speciesAngle / 8.0f);
    }

    // Sème les plaines `keys` (caméra au centre de la planète) et relit leurs arbres.
    bool Sow(RHI::Device& device, const Planetology::Terrain& terrain, const PlanetComponent& planet, const VegetationSet& biomes,
             std::span<const Planetology::TileKey> keys, std::vector<std::vector<VegetationRenderer::Tree>>& trees) {
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
        (*renderer)->SetPlanet(static_cast<Entity>(1), terrain, planet, VegetationComponent{}, biomes, Math::Quaternionf{}, plains);
        (*renderer)->EndFrame(0, {{0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 1.0f, 1.0f}, {}, {}, {});
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
        return GpuMissing("GPU", device.error().c_str());
    }

    // Partout chaud et humide, ni mer ni neige à portée : seuls les biomes, les forêts et la pente décident.
    PlanetComponent planet;
    planet.radius = 60000.0f;
    planet.relief = 800.0f;
    planet.seed = 5;
    planet.temperature = 25.0f;
    planet.poleTemperature = 25.0f;
    planet.humidity = 1.0f;
    planet.seaLevel = -200.0f;
    planet.snowLevel = 1000.0f;
    VegetationSet everywhere;
    everywhere.species = {Species(0.7f, 1.3f, 90.0f)};
    AddBiome(everywhere, {1.0f, 1.0f, 1.0f}, 1.0f, 1.0f, {{0, 1.0f}});
    const Planetology::Terrain terrain(planet.radius, planet.relief, planet.seed, planet.mountainWidth);
    const std::uint32_t level = VegetationRenderer::GetPlainLevel(terrain);
    CHECK(level > 0 && level < terrain.GetMaxLevel());
    const std::vector<Planetology::TileKey> keys{Planetology::FindTile(Math::Normalize(Math::Vector3d{0.3, 0.8, -0.5}), level),
                                                 Planetology::FindTile(Math::Normalize(Math::Vector3d{-0.9, -0.2, 0.4}), level)};

    std::vector<std::vector<VegetationRenderer::Tree>> first;
    CHECK(Sow(**device, terrain, planet, everywhere, keys, first));
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
            CHECK(tree.scale >= 0.7f && tree.scale <= 1.3f);
            CHECK(SpeciesOf(tree) == 0 && tree.speciesAngle >= 0.0f && tree.speciesAngle < 6.3f);
        }
        // Le float de la position (à quelques centaines de mètres du point de référence) et celui du relief.
        if (worst > 0.01) {
            std::printf("ÉCHEC plaine %zu : arbre à %.3f m du sol\n", k, worst);
            return false;
        }
    }

    // Mêmes places et mêmes arbres une seconde fois (l'ordre, lui, dépend du GPU).
    std::vector<std::vector<VegetationRenderer::Tree>> second;
    CHECK(Sow(**device, terrain, planet, everywhere, keys, second));
    const auto byPlace = [](const VegetationRenderer::Tree& a, const VegetationRenderer::Tree& b) {
        return a.position.x != b.position.x ? a.position.x < b.position.x : a.position.z < b.position.z;
    };
    for (std::size_t k = 0; k < keys.size(); ++k) {
        CHECK(second[k].size() == first[k].size());
        std::ranges::sort(first[k], byPlace);
        std::ranges::sort(second[k], byPlace);
        for (std::size_t t = 0; t < first[k].size(); ++t) {
            CHECK(first[k][t].position == second[k][t].position && first[k][t].scale == second[k][t].scale
                  && first[k][t].speciesAngle == second[k][t].speciesAngle);
        }
    }

    // Aucune pente permise : plus aucun arbre ; forêts à moitié : moins d'arbres.
    VegetationSet flat = everywhere;
    flat.species[0].maxSlope = 0.0f;
    CHECK(Sow(**device, terrain, planet, flat, keys, second));
    CHECK(second[0].empty() && second[1].empty());
    VegetationSet half;
    half.species = everywhere.species;
    AddBiome(half, {1.0f, 1.0f, 1.0f}, 0.5f, 1.0f, {{0, 1.0f}});
    CHECK(Sow(**device, terrain, planet, half, keys, second));
    CHECK(second[0].size() + second[1].size() < first[0].size() + first[1].size());

    // Parts des espèces d'un biome : un quart de l'une, trois quarts de l'autre (de tailles différentes).
    VegetationSet mixed;
    mixed.species = {Species(0.7f, 1.3f, 90.0f), Species(2.0f, 3.0f, 90.0f)};
    AddBiome(mixed, {1.0f, 1.0f, 1.0f}, 1.0f, 1.0f, {{0, 1.0f}, {1, 3.0f}});
    CHECK(Sow(**device, terrain, planet, mixed, keys, second));
    std::size_t total = 0;
    std::size_t large = 0;
    for (const auto& plain : second) {
        for (const VegetationRenderer::Tree& tree : plain) {
            ++total;
            const bool big = SpeciesOf(tree) == 1;
            large += big ? 1 : 0;
            CHECK(big ? tree.scale >= 2.0f && tree.scale <= 3.0f : tree.scale >= 0.7f && tree.scale <= 1.3f);
        }
    }
    const double part = static_cast<double>(large) / static_cast<double>(std::max<std::size_t>(total, 1));
    CHECK(total > 1000 && part > 0.72 && part < 0.78);

    // Zones du relief : une espèce des montagnes, une des plaines, sur des plaines plus ou moins au pied d'une chaîne
    // (cherchées avec Terrain::Zones, sans collines). Chaque arbre est dans une zone de son biome, et chaque biome
    // prend sa part du sol : les montagnes m, les plaines (1 - m) de ce qui reste, 1 - m.
    VegetationSet zoned;
    zoned.species = {Species(0.7f, 1.3f, 90.0f), Species(0.7f, 1.3f, 90.0f)};
    AddBiome(zoned, {0.0f, 0.0f, 1.0f}, 1.0f, 1.0f, {{0, 1.0f}});
    AddBiome(zoned, {1.0f, 0.0f, 0.0f}, 1.0f, 1.0f, {{1, 1.0f}});
    constexpr std::array<std::array<double, 2>, 3> buckets{{{0.1, 0.3}, {0.4, 0.6}, {0.7, 0.9}}};
    std::vector<Planetology::TileKey> edges;
    std::vector<double> expected;
    for (const auto& [low, high] : buckets) {
        for (int i = 0; i < 8000; ++i) {
            const double a = i * 2.399963;
            const double y = 1.0 - (i + 0.5) / 4000.0;
            const double r = std::sqrt(1.0 - y * y);
            const Math::Vector3d direction{r * std::cos(a), y, r * std::sin(a)};
            Planetology::Terrain::Zones zones;
            terrain.GetHeight(direction, octaves, &zones);
            if (zones.mountains > low && zones.mountains < high && zones.hills < 0.02) {
                edges.push_back(Planetology::FindTile(direction, level));
                const double m = zones.mountains;
                expected.push_back(m / (m + (1.0 - m) * (1.0 - m)));
                break;
            }
        }
    }
    CHECK(edges.size() == buckets.size());
    CHECK(Sow(**device, terrain, planet, zoned, edges, second));
    for (std::size_t k = 0; k < edges.size(); ++k) {
        const Math::Vector3d origin = Planetology::GetTileBounds(terrain, edges[k]).origin;
        std::size_t mountain = 0;
        for (const VegetationRenderer::Tree& tree : second[k]) {
            Planetology::Terrain::Zones zones;
            terrain.GetHeight(Math::Normalize(origin + Math::Vector3d(tree.position)), octaves, &zones);
            const std::uint32_t species = SpeciesOf(tree);
            CHECK(species < 2);
            // Une zone de son biome : là où il ne prend rien, son poids est nul.
            CHECK(species == 0 ? zones.mountains > 0.0 : zones.mountains < 1.0);
            mountain += species == 0 ? 1 : 0;
        }
        CHECK(second[k].size() > 1000);
        // La zone change peu sur une plaine (quelques centaines de mètres) : sa valeur au point cherché suffit.
        const double part = static_cast<double>(mountain) / static_cast<double>(second[k].size());
        if (std::abs(part - expected[k]) > 0.1) {
            std::printf("ÉCHEC plaine %zu : %.2f d'arbres des montagnes, %.2f attendus\n", k, part, expected[k]);
            return false;
        }
    }
    return true;
}
