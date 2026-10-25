#pragma once

#include "Flux/Export.hpp"
#include "Flux/Planetology/Vegetation.hpp"
#include "Flux/Scene/Components.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Flux::Planetology {
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
        static constexpr std::uint32_t MaxSharesPerBiome = VegetationBiome::MaxSpecies;
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

    // Aperçus de l'éditeur (SetVegetationPreview) : le texte provisoire de path, s'il y en a un, et leur compteur, qui
    // change avec chacun.
    std::optional<std::string> FindVegetationPreview(std::string_view path);
    std::uint64_t GetVegetationPreviewRevision();

    // Lit les biomes d'une planète (VegetationComponent::biome1 à biome8) et leurs espèces, et les relit quand un
    // fichier change (vérifié une fois par seconde) ou qu'un aperçu de l'éditeur change (dès l'appel suivant).
    // Fichier faux ou absent : un message à chaque lecture, et il est ignoré. Garde les biomes de quelques planètes à
    // la fois (une liste de biomes chacune).
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
            std::uint64_t previews = 0; // GetVegetationPreviewRevision() à la dernière lecture
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
