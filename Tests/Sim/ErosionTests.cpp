// Carte d'érosion des planètes (ErosionMap) : même carte quel que soit le nombre de fils, l'eau de chaque maille
// arrive à une sortie, le débit se conserve, les vallées se creusent là où l'eau se rassemble, et le cache relit la
// même carte.
#include "Check.hpp"

#include "Flux/Planetology/ErosionMap.hpp"
#include "Flux/Planetology/Planet.hpp"
#include "Flux/Scene/Components.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>

using namespace Flux;
using Planetology::ErosionMap;

namespace {
    bool SameMap(const ErosionMap& a, const ErosionMap& b) {
        if (a.GetResolution() != b.GetResolution() || a.GetKey() != b.GetKey()) {
            return false;
        }
        for (std::uint32_t node = 0; node < a.GetNodeCount(); ++node) {
            const float ca = a.GetCarve(node), cb = b.GetCarve(node), fa = a.GetFlow(node), fb = b.GetFlow(node);
            if (std::memcmp(&ca, &cb, sizeof(float)) != 0 || std::memcmp(&fa, &fb, sizeof(float)) != 0
                || a.GetFlowDirection(node) != b.GetFlowDirection(node)) {
                return false;
            }
        }
        return true;
    }
}

bool RunErosionTests() {
    const double radius = 60000.0;
    const double relief = 3000.0;
    const Planetology::Terrain terrain(radius, relief, 5);
    Planetology::ErosionSettings settings;
    settings.sea = true;
    settings.seaHeight = -0.1 * relief;

    // Même carte avec 1 fil ou 4.
    const ErosionMap one = ErosionMap::Build(terrain, settings, 1);
    const ErosionMap four = ErosionMap::Build(terrain, settings, 4);
    CHECK(one.GetLevel() == ErosionMap::GetLevel(terrain, settings));
    CHECK(one.GetResolution() == 128);
    CHECK(SameMap(one, four));
    const std::uint32_t count = one.GetNodeCount();
    CHECK(count == 6 * 129 * 129);

    // Représentants : un point d'arête a la même direction sur chaque face qui le porte, et ses copies les mêmes valeurs.
    std::uint32_t canonicals = 0;
    for (std::uint32_t node = 0; node < count; ++node) {
        const std::uint32_t canonical = one.GetCanonical(node);
        CHECK(canonical <= node);
        canonicals += canonical == node ? 1u : 0u;
        CHECK(Math::Length(one.GetNodeDirection(node) - one.GetNodeDirection(canonical)) < 1e-9);
        CHECK(one.GetCarve(node) == one.GetCarve(canonical) && one.GetFlow(node) == one.GetFlow(canonical));
    }
    // Sommets d'un cube subdivisé : 6n² + 2.
    CHECK(canonicals == 6 * 128 * 128 + 2);

    // Voisinage symétrique, coutures comprises.
    std::uint32_t corners = 0;
    for (std::uint32_t node = 0; node < count; ++node) {
        if (one.GetCanonical(node) != node) {
            continue;
        }
        std::uint32_t neighbors[8];
        one.GetNeighbors(node, neighbors);
        std::uint32_t valid = 0;
        for (const std::uint32_t other : neighbors) {
            if (other == ErosionMap::NoNode) {
                continue;
            }
            ++valid;
            CHECK(other < count && other != node);
            std::uint32_t back[8];
            one.GetNeighbors(other, back);
            CHECK(std::find(back, back + 8, node) != back + 8);
        }
        // 8 voisins, 6 aux huit coins du cube.
        CHECK(valid == 8 || valid == 6);
        corners += valid == 6 ? 1u : 0u;
    }
    CHECK(corners == 8);

    // L'eau de chaque maille arrive à une sortie (pas de boucle), le débit grossit vers l'aval et se conserve.
    double outletFlow = 0.0;
    std::uint32_t outlets = 0;
    for (std::uint32_t node = 0; node < count; ++node) {
        if (one.GetCanonical(node) != node) {
            continue;
        }
        std::uint32_t at = node;
        std::uint32_t steps = 0;
        while (one.GetReceiver(at) != at && steps <= count) {
            const std::uint32_t down = one.GetReceiver(at);
            CHECK(one.GetFlow(down) >= one.GetFlow(at) + 0.99f * static_cast<float>(settings.rain));
            at = down;
            ++steps;
        }
        CHECK(steps <= count);
        if (one.GetFlowDirection(node) == ErosionMap::Outlet) {
            outletFlow += one.GetFlow(node);
            ++outlets;
        }
    }
    CHECK(outlets > 0);
    CHECK(std::abs(outletFlow - canonicals * settings.rain) < 1e-3 * canonicals);

    // Creusement borné, nul en mer ; des vallées là où l'eau se rassemble, presque rien sur les crêtes.
    double riverCarve = 0.0, ridgeCarve = 0.0;
    std::uint32_t rivers = 0, ridges = 0;
    float deepest = 0.0f;
    for (std::uint32_t node = 0; node < count; ++node) {
        const float carve = one.GetCarve(node);
        CHECK(carve <= 0.0f && carve >= -static_cast<float>(settings.valleyDepth * relief) - 0.01f);
        deepest = std::min(deepest, carve);
        if (one.GetFlow(node) >= 200.0f) {
            riverCarve += carve;
            ++rivers;
        } else if (one.GetFlow(node) <= 2.0f) {
            ridgeCarve += carve;
            ++ridges;
        }
    }
    CHECK(rivers > 0 && ridges > 0);
    riverCarve /= rivers;
    ridgeCarve /= ridges;
    std::printf("  érosion 60 km : %u sorties, creusement moyen rivières %.0f m, crêtes %.1f m, au plus %.0f m\n", outlets,
                riverCarve, ridgeCarve, deepest);
    CHECK(riverCarve < -50.0 && ridgeCarve > -1.0);

    // Interpolée : la valeur du point en ce point, et sans saut aux coutures des faces.
    for (std::uint32_t node = 0; node < count; node += 97) {
        const ErosionMap::Sample sample = one.Get(one.GetNodeDirection(node));
        CHECK(std::abs(sample.carve - one.GetCarve(node)) < 1e-2);
    }
    for (std::uint32_t k = 0; k < 200; ++k) {
        const double t = -1.0 + 2.0 * (k + 0.5) / 200.0;
        const ErosionMap::Sample a = one.Get(Planetology::DirectionFromFace(0, 1.0 - 1e-9, t));
        const ErosionMap::Sample b = one.Get(Planetology::DirectionFromFace(0, 1.0 + 1e-9, t));
        CHECK(std::abs(a.carve - b.carve) < 1e-2);
    }

    // Planète sans mer : toute l'eau finit au point le plus bas.
    Planetology::ErosionSettings dry;
    dry.iterations = 4;
    const ErosionMap desert = ErosionMap::Build(terrain, dry, 4);
    std::uint32_t dryOutlets = 0;
    for (std::uint32_t node = 0; node < desert.GetNodeCount(); ++node) {
        dryOutlets += desert.GetCanonical(node) == node && desert.GetFlowDirection(node) == ErosionMap::Outlet ? 1u : 0u;
    }
    CHECK(dryOutlets == 1);
    CHECK(ErosionMap::GetKey(terrain, dry) != ErosionMap::GetKey(terrain, settings));
    CHECK(ErosionMap::GetKey(Planetology::Terrain(radius, relief, 6), settings) != ErosionMap::GetKey(terrain, settings));

    // Cache : relu à l'identique, refusé sous une autre clé, refait par LoadOrBuild s'il est abîmé.
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "flux-erosion-tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    const ErosionMap built = ErosionMap::LoadOrBuild(terrain, settings, directory, 4);
    CHECK(SameMap(built, one));
    const std::filesystem::path file = directory / std::format("erosion-{:016x}.bin", one.GetKey());
    CHECK(std::filesystem::exists(file));
    ErosionMap loaded;
    CHECK(loaded.Load(file, one.GetKey()));
    CHECK(SameMap(loaded, one));
    CHECK(!loaded.Load(file, one.GetKey() + 1));
    std::filesystem::resize_file(file, 100, error);
    CHECK(!loaded.Load(file, one.GetKey()));
    CHECK(SameMap(ErosionMap::LoadOrBuild(terrain, settings, directory, 4), one));
    std::filesystem::remove_all(directory, error);

    // Terrain d'une planète avec ErosionComponent : la carte (calculée une fois, partagée) s'ajoute au relief ; ce
    // que reçoit le GPU pour une tuile est ce que GetHeight ajoute à ses sommets.
    PlanetComponent planet;
    planet.radius = static_cast<float>(radius);
    planet.relief = static_cast<float>(relief);
    planet.seed = 5;
    planet.ocean = true;
    planet.seaLevel = -10.0f;
    ErosionComponent erosion;
    CHECK(!Planetology::GetReliefShape(planet).eroded && Planetology::GetReliefShape(planet, nullptr, &erosion).eroded);
    CHECK(Planetology::GetCraterDepth(planet, nullptr, &erosion) == static_cast<double>(erosion.valleyDepth) * relief);
    const auto plain = Planetology::MakeTerrain(planet, nullptr, nullptr, false);
    CHECK(plain && !plain->GetErosion());
    const auto carved = Planetology::MakeTerrain(planet, nullptr, &erosion, true);
    const Planetology::ErosionSettings valleys = Planetology::GetReliefShape(planet, nullptr, &erosion).valleys;
    CHECK(valleys.sea && valleys.seaHeight == -0.1 * relief && valleys.valleyDepth == static_cast<double>(erosion.valleyDepth));
    CHECK(carved && carved->GetErosion() && carved->GetErosion()->GetKey() == ErosionMap::GetKey(Planetology::Terrain(planet), valleys));
    CHECK(Planetology::MakeTerrain(planet, nullptr, &erosion, false)->GetErosion() == carved->GetErosion());
    const ErosionMap& map = *carved->GetErosion();
    std::uint32_t river = 0;
    for (std::uint32_t node = 0; node < map.GetNodeCount(); ++node) {
        river = map.GetCarve(node) < map.GetCarve(river) ? node : river;
    }
    // Une tuile sur la rivière la plus creusée, une au bord d'une face (rangées prises sur la voisine), une racine.
    const std::uint32_t edgeLevel = map.GetLevel() + 1;
    const Planetology::TileKey keys[] = {Planetology::FindTile(map.GetNodeDirection(river), map.GetLevel() + 2),
                                         {2, static_cast<std::uint8_t>(edgeLevel), (1u << edgeLevel) - 1u, 1u},
                                         {4, 0, 0, 0}};
    auto inputs = std::make_unique<Planetology::TileGpuInputs>();
    for (const Planetology::TileKey& key : keys) {
        Planetology::PrepareTileGpu(*carved, key, *inputs);
        CHECK(key.level != keys[0].level || inputs->wetness[3] == 1);
        const std::uint32_t octaves = carved->GetOctaves(key.level);
        const double cells = 32.0 * static_cast<double>(1u << key.level);
        const auto at = [&](std::uint32_t tile, std::uint32_t k) { return -1.0 + 2.0 * (32.0 * tile + k - 2.0) / cells; };
        for (std::uint32_t j = 0; j < Planetology::TileGpuInputs::Columns; ++j) {
            for (std::uint32_t i = 0; i < Planetology::TileGpuInputs::Columns; ++i) {
                const Math::Vector3d d = Planetology::DirectionFromFace(key.face, at(key.x, i), at(key.y, j));
                const double added = carved->GetHeight(d, octaves) - carved->GetReliefHeight(d, octaves);
                const double sent = inputs->wetness[3] ? inputs->carve[j * Planetology::TileGpuInputs::Columns + i] : 0.0;
                CHECK(std::abs(added - sent) < 1e-3);
            }
        }
    }

    // Temps de calcul d'une planète de 200 km (256 mailles par face), pour suivre le coût.
    const Planetology::Terrain large(200000.0, 4500.0, 3);
    Planetology::ErosionSettings largeSettings;
    largeSettings.sea = true;
    largeSettings.seaHeight = -0.1 * 4500.0;
    const auto begin = std::chrono::steady_clock::now();
    const ErosionMap big = ErosionMap::Build(large, largeSettings);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::printf("  érosion 200 km : %u mailles par face, %.2f s\n", big.GetResolution(), seconds);
    CHECK(big.GetResolution() == 256);
    return true;
}
