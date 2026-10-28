#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Color.hpp"

#include <cstdint>
#include <expected>
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
        // Tronc pour la collision (capsule droite du pied jusqu'à trunkHeight), en mètres à la taille du modèle.
        float trunkRadius = 0.35f, trunkHeight = 20.0f;
        Math::LinearColor canopyColor{0.02f, 0.045f, 0.012f, 1.0f}; // cimes vues de loin
    };

    // Un biome (fichier .biome.json) : où il pousse et quelles espèces.
    struct VegetationBiome
    {
        static constexpr std::uint32_t MaxSpecies = 4; // shaders/PlanetClimate.slang
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
        std::vector<Share> species; // MaxSpecies au plus
    };

    // Lecture : valeurs absentes prises par défaut, valeurs hors limites ramenées dedans.
    FLUX_API std::expected<TreeSpecies, std::string> ParseTreeSpecies(std::string_view text);
    FLUX_API std::expected<VegetationBiome, std::string> ParseVegetationBiome(std::string_view text);
    // Écriture (éditeur) : relue par Parse… à l'identique.
    FLUX_API std::string WriteTreeSpecies(const TreeSpecies& species);
    FLUX_API std::string WriteVegetationBiome(const VegetationBiome& biome);

    // Aperçu d'un réglage pas encore enregistré (éditeur) : text est lu à la place du fichier path (relatif au dossier
    // des assets, avec des « / ») par les planètes, dès l'image suivante. Texte vide : le fichier redevient la
    // référence. Chaque appel fait resemer les arbres visibles : à espacer pendant un glisser.
    FLUX_API void SetVegetationPreview(std::string_view path, std::string text);
}
