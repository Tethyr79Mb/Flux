// Espèces et biomes de la végétation (Planetology::VegetationLibrary) : lecture des fichiers, biomes livrés avec le jeu
// (Game/Module/assets/Vegetation), et zones du relief qui en décident (Terrain::Zones).
#include "Check.hpp"

#include "Flux/Planetology/Planet.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Planetology/VegetationLibrary.hpp"

#include <cmath>
#include <filesystem>

using namespace Flux;

bool RunVegetationTests() {
    // Espèce : les modèles absents prennent le précédent ; valeurs hors limites ramenées.
    auto species = Planetology::ParseTreeSpecies(R"({"variants": [{"model": "a.glb"}, {"model": "b.glb", "lodModel": "b1.glb"}],
                                                    "minScale": 2, "maxScale": 1, "maxSlope": 120, "canopyColor": [0.1, 0.2, 0.3]})");
    CHECK(species && species->variants.size() == 2);
    CHECK(species->variants[0].lodModel == "a.glb" && species->variants[0].farModel == "a.glb");
    CHECK(species->variants[1].lodModel == "b1.glb" && species->variants[1].farModel == "b1.glb");
    CHECK(species->minScale == 2.0f && species->maxScale == 2.0f && species->maxSlope == 90.0f);
    CHECK(species->canopyColor.g == 0.2f);
    CHECK(!Planetology::ParseTreeSpecies(R"({"minScale": 1})"));
    CHECK(!Planetology::ParseTreeSpecies(R"({"variants": [{"lodModel": "a.glb"}]})"));
    CHECK(!Planetology::ParseTreeSpecies(R"({"variants": [{"model": "a.glb"}], "tint": "fort"})"));
    CHECK(!Planetology::ParseTreeSpecies("pas du JSON"));

    // Biome : conditions et espèces, 4 au plus.
    auto biome = Planetology::ParseVegetationBiome(R"({"zones": {"plains": 0, "mountains": 1}, "temperature": [-5, 10],
                                                       "species": [{"species": "e.espece.json", "share": 3}]})");
    CHECK(biome && biome->plains == 0.0f && biome->hills == 1.0f && biome->mountains == 1.0f);
    CHECK(biome->minTemperature == -5.0f && biome->maxTemperature == 10.0f && biome->species.size() == 1);
    CHECK(!Planetology::ParseVegetationBiome(R"({"temperature": [5]})"));
    CHECK(!Planetology::ParseVegetationBiome(R"({"species": [{"species": "a"}, {"species": "b"}, {"species": "c"},
                                                             {"species": "d"}, {"species": "e"}]})"));

    // Biomes livrés (ceux d'une nouvelle planète) : tous lus, leurs modèles présents.
    const std::filesystem::path assets = Platform::PathFromUtf8(FLUX_GAME_ASSETS);
    Planetology::VegetationLibrary library;
    const VegetationComponent vegetation;
    const Planetology::VegetationSet& set = library.Get(assets, vegetation);
    CHECK(set.biomes.size() == 6);
    CHECK(set.species.size() == 7); // épicéa, sapin, pin (deux variantes chacun), feuillu
    for (const Planetology::VegetationSet::Species& tree : set.species) {
        for (const std::string& model : {tree.models.model, tree.models.lodModel, tree.models.farModel}) {
            CHECK(std::filesystem::exists(assets / Platform::PathFromUtf8(model)));
        }
    }
    for (std::size_t b = 0; b < set.biomes.size(); ++b) {
        CHECK(set.biomeShares[b][3] == 1.0f);
        for (std::size_t s = 0; s < 4; ++s) {
            CHECK((set.biomeSpecies[b][s] & 255u) + (set.biomeSpecies[b][s] >> 8) <= set.species.size());
        }
    }
    // Désert (biome 2) sans arbres ; montagne (biome 3) : épicéas et sapins seulement, pas de plaines.
    CHECK(set.biomes[1].humidity[2] == 0.0f);
    CHECK(set.biomes[2].zones[0] == 0.0f && set.biomes[2].zones[2] == 1.0f);
    CHECK(set.biomeSpecies[2][0] >> 8 == 2 && set.biomeSpecies[2][1] >> 8 == 2);
    // Relu tel quel tant que rien ne change.
    const std::uint64_t revision = set.revision;
    CHECK(library.Get(assets, vegetation).revision == revision);
    VegetationComponent fewer;
    fewer.biome2.clear();
    CHECK(library.Get(assets, fewer).biomes.size() == 5);
    CHECK(library.Get(assets, vegetation).revision == revision); // la première planète est gardée

    // Zones du relief : de 0 à 1, sans changer la hauteur ; des montagnes et des plaines sur une planète.
    const Planetology::Terrain terrain(60000.0, 800.0, 5);
    const std::uint32_t octaves = terrain.GetOctaves(terrain.GetMaxLevel());
    int mountains = 0;
    int plains = 0;
    for (int i = 0; i < 400; ++i) {
        const double a = i * 2.399963;
        const double y = 1.0 - (i + 0.5) / 200.0;
        const double r = std::sqrt(1.0 - y * y);
        const Math::Vector3d direction{r * std::cos(a), y, r * std::sin(a)};
        Planetology::Terrain::Zones zones;
        CHECK(terrain.GetHeight(direction, octaves, &zones) == terrain.GetHeight(direction, octaves));
        CHECK(zones.hills >= 0.0 && zones.hills <= 1.0 && zones.mountains >= 0.0 && zones.mountains <= 1.0);
        mountains += zones.mountains > 0.9 ? 1 : 0;
        plains += zones.mountains < 0.01 && zones.hills < 0.01 ? 1 : 0;
    }
    CHECK(mountains > 4 && plains > 40);
    return true;
}
