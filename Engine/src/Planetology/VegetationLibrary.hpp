#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Scene/Components.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Flux::Planetology {
    // Une espèce d'arbre (fichier .espece.json) : à quoi il ressemble. Chaque variante a ses trois modèles (près,
    // simplifié, loin) ; le reste est commun.
    struct TreeSpecies
    {
        struct Variant
        {
            std::string model, lodModel, farModel; // relatifs au dossier des assets
        };
        std::vector<Variant> variants;
        float minScale = 0.7f, maxScale = 1.3f; // x le modèle
        float maxSlope = 30.0f;                 // degrés
        float tint = 0.1f;                      // écart de teinte d'un arbre à l'autre (0 : aucun)
        Math::LinearColor canopyColor{0.02f, 0.045f, 0.012f, 1.0f}; // cimes vues de loin
    };

    // Un biome (fichier .biome.json) : où il pousse et quelles espèces.
    struct VegetationBiome
    {
        float plains = 1.0f, hills = 1.0f, mountains = 1.0f; // part prise dans chaque zone du relief (Terrain::Zones)
        float minAltitude = -100.0f, maxAltitude = 100.0f;   // % du relief, au-dessus du niveau zéro
        float minTemperature = -100.0f, maxTemperature = 100.0f; // °C
        float minHumidity = -1.0f, maxHumidity = 2.0f;
        float cover = 0.5f;   // part couverte de forêts
        float density = 0.4f; // part des places occupée dans une forêt
        float scale = 1.0f;   // taille des arbres, x celle de l'espèce
        struct Share
        {
            std::string species; // chemin du .espece.json
            float share = 1.0f;
        };
        std::vector<Share> species; // 4 au plus
    };

    FLUX_API std::expected<TreeSpecies, std::string> ParseTreeSpecies(std::string_view text);
    FLUX_API std::expected<VegetationBiome, std::string> ParseVegetationBiome(std::string_view text);

    // Doit correspondre à Biome dans shaders/PlanetClimate.slang.
    struct BiomeGpu
    {
        std::array<float, 4> zones;
        std::array<float, 4> ranges;
        std::array<float, 4> humidity;
        std::array<float, 4> color;
    };

    // Biomes d'une planète prêts pour le GPU : les variantes de leurs espèces mises en une table (MaxSpecies au plus).
    struct VegetationSet
    {
        static constexpr std::uint32_t MaxBiomes = 8;   // shaders/PlanetClimate.slang
        static constexpr std::uint32_t MaxSpecies = 8;  // shaders/PlanetTile.slang, shaders/Vegetation.slang
        static constexpr std::uint32_t MaxSharesPerBiome = 4;
        struct Species
        {
            TreeSpecies::Variant models;
            float minScale, maxScale, maxSlope, tint;
        };
        std::vector<Species> species;
        std::vector<BiomeGpu> biomes;
        // Espèces de chaque biome : première variante dans species | nombre de variantes << 8 ; et leurs parts
        // cumulées (la dernière à 1).
        std::vector<std::array<std::uint32_t, 4>> biomeSpecies;
        std::vector<std::array<float, 4>> biomeShares;
        std::uint64_t revision = 0; // change avec un fichier lu
    };

    // Lit les biomes d'une planète (VegetationComponent::biome1 à biome8) et leurs espèces, et les relit quand un
    // fichier change (vérifié une fois par seconde). Fichier faux ou absent : un message à chaque lecture, et il est
    // ignoré. Garde les biomes de quelques planètes à la fois (une liste de biomes chacune).
    // Exporté pour les tests (Tests/src/VegetationTests.cpp).
    class FLUX_API VegetationLibrary {
        public:
        // Valable jusqu'à l'appel suivant.
        const VegetationSet& Get(const std::filesystem::path& assetsDirectory, const VegetationComponent& vegetation);

        private:
        struct Entry
        {
            std::filesystem::path directory;
            std::array<std::string, VegetationSet::MaxBiomes> paths;
            // Biomes et espèces lus (chemins relatifs aux assets) et leur date (min : absent).
            std::map<std::string, std::filesystem::file_time_type> files;
            std::chrono::steady_clock::time_point checked{};
            std::uint64_t lastUsed = 0;
            VegetationSet set;
        };
        static void Rebuild(Entry& entry);
        static bool Changed(const Entry& entry);

        std::vector<Entry> m_entries;
        std::uint64_t m_calls = 0;
        std::uint64_t m_revision = 0;
    };
}
