// Espèces et biomes de la végétation (Planetology::VegetationLibrary) : lecture des fichiers, biomes livrés avec le jeu
// (Game/Module/assets/Vegetation), et zones du relief qui en décident (Terrain::Zones).
#include "Check.hpp"

#include "Flux/Planetology/Planet.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Planetology/ReliefWetness.hpp"
#include "Planetology/TreeSowing.hpp"
#include "Planetology/VegetationLibrary.hpp"

#include <cmath>
#include <filesystem>
#include <vector>

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
    // Sol : tout convient par défaut ; roches et profondeur de la terre lues, ramenées dans [0, 1], relues à l'identique.
    CHECK(biome->granite == 1.0f && biome->basalt == 1.0f && biome->minSoil == 0.0f && biome->maxSoil == 1.0f);
    auto rocky = Planetology::ParseVegetationBiome(R"({"rocks": {"limestone": 0, "basalt": 3}, "soil": [0.4, 1.5]})");
    CHECK(rocky && rocky->limestone == 0.0f && rocky->basalt == 1.0f && rocky->granite == 1.0f);
    CHECK(rocky->minSoil == 0.4f && rocky->maxSoil == 1.0f);
    auto rockyAgain = Planetology::ParseVegetationBiome(Planetology::WriteVegetationBiome(*rocky));
    CHECK(rockyAgain && rockyAgain->limestone == 0.0f && rockyAgain->minSoil == 0.4f && rockyAgain->maxSoil == 1.0f);
    CHECK(!Planetology::ParseVegetationBiome(R"({"rocks": [1, 0]})"));
    CHECK(!Planetology::ParseVegetationBiome(R"({"understory": {"species": "a"}})"));
    CHECK(Planetology::ParseVegetationBiome(R"({"understoryDensity": 3})")->understoryDensity == 1.0f);
    CHECK(!Planetology::ParseVegetationBiome(R"({"species": [{"species": "a"}, {"species": "b"}, {"species": "c"},
                                                             {"species": "d"}, {"species": "e"}]})"));

    // Biomes livrés (ceux d'une nouvelle planète) : tous lus, leurs modèles présents.
    const std::filesystem::path assets = Platform::PathFromUtf8(FLUX_GAME_ASSETS);
    Planetology::VegetationLibrary library;
    const VegetationComponent vegetation;
    const Planetology::VegetationSet& set = library.Get(assets, vegetation);
    CHECK(set.biomes.size() == 6);
    CHECK(set.species.size() == 9); // épicéa, sapin, pin, fougère (deux variantes chacun), feuillu
    for (const Planetology::VegetationSet::Species& tree : set.species) {
        for (const std::string& model : {tree.models.model, tree.models.lodModel, tree.models.farModel}) {
            CHECK(std::filesystem::exists(assets / Platform::PathFromUtf8(model)));
        }
        // Une litière pour chaque arbre, aucune pour la fougère (une plante).
        CHECK(tree.plant == tree.litter.empty());
        CHECK(tree.litter.empty() || std::filesystem::exists(assets / Platform::PathFromUtf8(tree.litter)));
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
    // Sol sans borne au GPU quand le biome ne limite rien (montagne), bornée sinon.
    CHECK(set.biomes[2].soil[0] < 0.0f && set.biomes[2].soil[1] > 1.0f);
    CHECK(set.biomeSpecies[2][0] >> 8 == 2 && set.biomeSpecies[2][1] >> 8 == 2);
    CHECK(set.biomes[2].color[3] == 200.0f); // taille des bosquets absente du fichier : par défaut
    // Sous-bois de la montagne : les mêmes variantes que ses arbres (jeunes épicéas et sapins), sans en ajouter.
    CHECK(set.biomes[2].soil[2] > 0.0f && set.biomes[2].soil[3] < 0.5f);
    CHECK(set.understorySpecies[2][0] == set.biomeSpecies[2][0] && set.understorySpecies[2][1] == set.biomeSpecies[2][1]);
    CHECK(set.understoryShares[2][3] == 1.0f);
    CHECK(set.biomes[1].soil[2] == 0.0f); // désert : ni arbres ni sous-bois
    // Relu tel quel tant que rien ne change.
    const std::uint64_t revision = set.revision;
    CHECK(library.Get(assets, vegetation).revision == revision);
    VegetationComponent fewer;
    fewer.biome2.clear();
    CHECK(library.Get(assets, fewer).biomes.size() == 5);
    CHECK(library.Get(assets, vegetation).revision == revision); // la première planète est gardée

    // Écrits par l'éditeur : relus à l'identique, chemins accentués et guillemets compris.
    Planetology::VegetationBiome written;
    written.plains = 0.25f;
    written.minTemperature = -3.5f;
    written.maxHumidity = 0.7f;
    written.grove = 350.0f;
    written.species = {{"Végétation/a \"b\".espece.json", 60.0f}, {"c.espece.json", 40.0f}};
    written.understoryDensity = 0.35f;
    written.understoryScale = 0.25f;
    written.understory = {{"c.espece.json", 2.0f}};
    auto reread = Planetology::ParseVegetationBiome(Planetology::WriteVegetationBiome(written));
    CHECK(reread && reread->understoryDensity == 0.35f && reread->understoryScale == 0.25f && reread->understory.size() == 1
          && reread->understory[0].species == "c.espece.json" && reread->understory[0].share == 2.0f);
    CHECK(reread && reread->plains == 0.25f && reread->minTemperature == -3.5f && reread->maxHumidity == 0.7f && reread->grove == 350.0f);
    CHECK(reread->species.size() == 2 && reread->species[0].species == written.species[0].species && reread->species[1].share == 40.0f);
    CHECK(Planetology::WriteVegetationBiome(*reread) == Planetology::WriteVegetationBiome(written));
    CHECK(Planetology::ParseVegetationBiome(Planetology::WriteVegetationBiome({}))); // sans espèce : un désert
    Planetology::TreeSpecies tree;
    tree.variants = {{"a.glb", "a1.glb", "a2.glb"}, {"b.glb", "b.glb", "b.glb"}};
    tree.maxSlope = 42.5f;
    tree.canopyColor = {0.023f, 0.051f, 0.013f, 1.0f};
    tree.litter = "Modèles/litière.glb";
    tree.plant = true;
    auto rereadTree = Planetology::ParseTreeSpecies(Planetology::WriteTreeSpecies(tree));
    CHECK(rereadTree && rereadTree->variants.size() == 2 && rereadTree->variants[0].farModel == "a2.glb");
    CHECK(rereadTree->maxSlope == 42.5f && rereadTree->canopyColor.g == 0.051f);
    CHECK(rereadTree->litter == tree.litter && rereadTree->plant);
    CHECK(!Planetology::ParseTreeSpecies(R"({"variants": [{"model": "a.glb"}]})")->plant);
    CHECK(!Planetology::ParseTreeSpecies(R"({"variants": [{"model": "a.glb"}], "plant": 1})"));
    CHECK(Planetology::WriteTreeSpecies(*rereadTree) == Planetology::WriteTreeSpecies(tree));

    // Aperçu de l'éditeur : lu à la place du fichier dès l'appel suivant, puis le fichier redevient la référence.
    Planetology::VegetationBiome bare = *Planetology::ParseVegetationBiome(Planetology::WriteVegetationBiome({}));
    bare.plains = 0.5f;
    Planetology::SetVegetationPreview(vegetation.biome3, Planetology::WriteVegetationBiome(bare));
    const Planetology::VegetationSet& previewed = library.Get(assets, vegetation);
    CHECK(previewed.revision != revision && previewed.biomes[2].zones[0] == 0.5f);
    Planetology::SetVegetationPreview(vegetation.biome3, {});
    CHECK(library.Get(assets, vegetation).biomes[2].zones[0] == 0.0f);

    // Zones du relief : de 0 à 1, sans changer la hauteur ; des montagnes et des plaines sur une planète.
    const Planetology::Terrain terrain(60000.0, 800.0, 5);

    // Sous-bois (SowPlain) : sur une planète tempérée partout couverte de forêt, des jeunes arbres en plus des arbres,
    // marqués et plus petits, jamais au pied d'un arbre, plus nombreux avec une densité plus forte ; sans densité, aucun.
    {
        PlanetComponent planet;
        planet.radius = 60000.0f;
        planet.relief = 800.0f;
        planet.seed = 5;
        planet.temperature = 15.0f;
        planet.poleTemperature = 15.0f;
        planet.seaLevel = -200.0f;
        planet.snowLevel = 1000.0f;
        const Planetology::Terrain world(planet);
        Planetology::VegetationSet forest;
        forest.species = {{{"a.glb", "a.glb", "a.glb"}, 1.0f, 1.0f, 90.0f, 0.0f, 0.3f, 10.0f, 12.0f, {}, false}};
        forest.biomes.push_back({{1.0f, 1.0f, 1.0f, 1.0f}, {-10.0f, 10.0f, -1000.0f, 1000.0f}, {-10.0f, 10.0f, 1.0f, 1.0f},
                                 {0.0f, 0.0f, 0.0f, 200.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {-1.0f, 2.0f, 0.3f, 0.25f}});
        forest.biomeSpecies.push_back({1u << 8, 1u << 8, 1u << 8, 1u << 8});
        forest.biomeShares.push_back({1.0f, 1.0f, 1.0f, 1.0f});
        forest.understorySpecies = forest.biomeSpecies;
        forest.understoryShares = forest.biomeShares;
        const Planetology::SowSettings settings = Planetology::GetSowSettings(world, planet);
        const Planetology::TileKey plain = Planetology::FindTile(Math::Normalize(Math::Vector3d{0.3, 0.8, -0.5}), Planetology::GetPlainLevel(world));
        const auto count = [&](const Planetology::VegetationSet& set, std::size_t& trees, std::size_t& young) {
            std::vector<Planetology::SownTree> sown;
            Planetology::SowPlain(world, settings, set, plain, sown);
            trees = young = 0;
            for (const Planetology::SownTree& tree : sown) {
                (tree.understory ? young : trees) += 1;
                if (tree.understory) {
                    CHECK(tree.scale == 0.25f);
                    for (const Planetology::SownTree& other : sown) {
                        CHECK(other.understory || Math::Length(other.position - tree.position) > 1.0);
                    }
                }
            }
            return true;
        };
        std::size_t trees, young;
        CHECK(count(forest, trees, young));
        CHECK(trees > 100 && young > 300);
        Planetology::VegetationSet denser = forest;
        denser.biomes[0].soil[2] = 0.6f;
        std::size_t denserTrees, denserYoung;
        CHECK(count(denser, denserTrees, denserYoung));
        CHECK(denserTrees == trees && denserYoung > young * 3 / 2);
        Planetology::VegetationSet bare = forest;
        bare.biomes[0].soil[2] = 0.0f;
        std::size_t bareTrees, bareYoung;
        CHECK(count(bare, bareTrees, bareYoung));
        CHECK(bareTrees == trees && bareYoung == 0);
        // Plante du sous-bois : sa taille à elle (pas understoryScale), les arbres n'en changent pas.
        Planetology::VegetationSet plants = forest;
        plants.species[0].plant = true;
        std::vector<Planetology::SownTree> sown;
        Planetology::SowPlain(world, settings, plants, plain, sown);
        std::size_t plantCount = 0;
        for (const Planetology::SownTree& placed : sown) {
            CHECK(placed.scale == 1.0f);
            plantCount += placed.understory ? 1 : 0;
            // Normale du sol : unitaire, du côté du haut.
            CHECK(std::abs(Math::Length(placed.ground) - 1.0f) < 1.0e-4f
                  && Math::Dot(Math::Vector3d(placed.ground), Math::Normalize(placed.position)) > 0.0);
        }
        CHECK(plantCount == young);
    }
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

    // Humidité du relief : de -1 (crête) à 1 (creux), creux et crêtes présents ; continue d'une plaine à sa voisine
    // (mêmes points sur le bord commun) ; aucune grille sur les tuiles trop grandes pour voir les vallons.
    CHECK(Planetology::GetWetnessLayout(terrain, 0).side == 0);
    const std::uint32_t plainLevel = Planetology::GetPlainLevel(terrain);
    const Planetology::WetnessLayout layout = Planetology::GetWetnessLayout(terrain, plainLevel);
    CHECK(layout.side >= 2 && layout.fade == 1.0f && layout.octaves < octaves);
    int hollows = 0;
    int ridges = 0;
    for (int i = 0; i < 40; ++i) {
        const double a = i * 2.399963;
        const double y = 0.9 - (i + 0.5) / 22.0;
        const double r = std::sqrt(1.0 - y * y);
        Planetology::TileKey key = Planetology::FindTile({r * std::cos(a), y, r * std::sin(a)}, plainLevel);
        key.x = std::min(key.x, (1u << plainLevel) - 2u);
        Planetology::WetnessGrid grid, next;
        Planetology::BuildWetnessGrid(terrain, key, grid);
        Planetology::BuildWetnessGrid(terrain, {key.face, key.level, key.x + 1, key.y}, next);
        for (std::uint32_t k = 0; k <= Planetology::TileQuads; k += 4) {
            const double wet = Planetology::SampleWetness(grid, Planetology::TileQuads, k);
            CHECK(wet > -1.0 && wet < 1.0);
            CHECK(std::abs(wet - Planetology::SampleWetness(next, 0.0, k)) < 1.0e-9);
            hollows += wet > 0.2 ? 1 : 0;
            ridges += wet < -0.2 ? 1 : 0;
        }
    }
    CHECK(hollows > 10 && ridges > 10);
    return true;
}
