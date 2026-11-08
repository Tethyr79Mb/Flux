#include "Flux/Planetology/Vegetation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace Flux::Planetology {
    namespace {
        using Json = nlohmann::json;

        // Lit document[key] dans value s'il y est ; faux s'il n'est pas du bon type.
        bool ReadNumber(const Json& document, const char* key, float& value, std::string& error) {
            const auto found = document.find(key);
            if (found == document.end()) {
                return true;
            }
            if (!found->is_number()) {
                error = std::format("« {} » doit être un nombre", key);
                return false;
            }
            value = found->get<float>();
            return true;
        }

        // [min, max] : deux nombres.
        bool ReadRange(const Json& document, const char* key, float& low, float& high, std::string& error) {
            const auto found = document.find(key);
            if (found == document.end()) {
                return true;
            }
            if (!found->is_array() || found->size() != 2 || !(*found)[0].is_number() || !(*found)[1].is_number()) {
                error = std::format("« {} » doit être [minimum, maximum]", key);
                return false;
            }
            low = (*found)[0].get<float>();
            high = (*found)[1].get<float>();
            return true;
        }

        bool ReadString(const Json& document, const char* key, std::string& value, std::string& error) {
            const auto found = document.find(key);
            if (found == document.end()) {
                return true;
            }
            if (!found->is_string()) {
                error = std::format("« {} » doit être un texte", key);
                return false;
            }
            value = found->get<std::string>();
            return true;
        }

        std::expected<Json, std::string> ParseObject(std::string_view text) {
            Json document = Json::parse(text.begin(), text.end(), nullptr, false);
            if (document.is_discarded() || !document.is_object()) {
                return std::unexpected("JSON illisible (attendu : un objet { ... })");
            }
            return document;
        }
    }

    std::expected<TreeSpecies, std::string> ParseTreeSpecies(std::string_view text) {
        auto document = ParseObject(text);
        if (!document) {
            return std::unexpected(document.error());
        }
        TreeSpecies species;
        std::string error;
        const auto variants = document->find("variants");
        if (variants == document->end() || !variants->is_array() || variants->empty()) {
            return std::unexpected("« variants » doit lister au moins une variante { \"model\": ... }");
        }
        for (const Json& entry : *variants) {
            TreeSpecies::Variant variant;
            if (!entry.is_object() || !ReadString(entry, "model", variant.model, error) || !ReadString(entry, "lodModel", variant.lodModel, error)
                || !ReadString(entry, "farModel", variant.farModel, error)) {
                return std::unexpected(error.empty() ? "variante : un objet { \"model\": ... } attendu" : error);
            }
            if (variant.model.empty()) {
                return std::unexpected("variante sans « model »");
            }
            // Sans simplifié ni lointain : le précédent sert.
            if (variant.lodModel.empty()) {
                variant.lodModel = variant.model;
            }
            if (variant.farModel.empty()) {
                variant.farModel = variant.lodModel;
            }
            species.variants.push_back(std::move(variant));
        }
        if (!ReadNumber(*document, "minScale", species.minScale, error) || !ReadNumber(*document, "maxScale", species.maxScale, error)
            || !ReadNumber(*document, "maxSlope", species.maxSlope, error) || !ReadNumber(*document, "tint", species.tint, error)
            || !ReadNumber(*document, "trunkRadius", species.trunkRadius, error)
            || !ReadNumber(*document, "trunkHeight", species.trunkHeight, error)
            || !ReadNumber(*document, "spacing", species.spacing, error)) {
            return std::unexpected(error);
        }
        if (const auto color = document->find("canopyColor"); color != document->end()) {
            if (!color->is_array() || color->size() != 3 || !std::ranges::all_of(*color, [](const Json& c) { return c.is_number(); })) {
                return std::unexpected("« canopyColor » doit être [rouge, vert, bleu] (linéaire, 0 à 1)");
            }
            species.canopyColor = {(*color)[0].get<float>(), (*color)[1].get<float>(), (*color)[2].get<float>(), 1.0f};
        }
        species.minScale = std::clamp(species.minScale, 0.05f, 20.0f);
        species.maxScale = std::clamp(species.maxScale, species.minScale, 20.0f);
        species.maxSlope = std::clamp(species.maxSlope, 0.0f, 90.0f);
        species.tint = std::clamp(species.tint, 0.0f, 0.5f);
        species.trunkRadius = std::clamp(species.trunkRadius, 0.0f, 10.0f);
        species.trunkHeight = std::clamp(species.trunkHeight, 0.0f, 200.0f);
        species.spacing = std::clamp(species.spacing, 1.0f, 100.0f);
        return species;
    }

    std::expected<VegetationBiome, std::string> ParseVegetationBiome(std::string_view text) {
        auto document = ParseObject(text);
        if (!document) {
            return std::unexpected(document.error());
        }
        VegetationBiome biome;
        std::string error;
        if (const auto zones = document->find("zones"); zones != document->end()) {
            if (!zones->is_object() || !ReadNumber(*zones, "plains", biome.plains, error) || !ReadNumber(*zones, "hills", biome.hills, error)
                || !ReadNumber(*zones, "mountains", biome.mountains, error)) {
                return std::unexpected(error.empty() ? "« zones » doit être { \"plains\": 1, \"hills\": 1, \"mountains\": 1 }" : error);
            }
        }
        if (!ReadRange(*document, "altitude", biome.minAltitude, biome.maxAltitude, error)
            || !ReadRange(*document, "temperature", biome.minTemperature, biome.maxTemperature, error)
            || !ReadRange(*document, "humidity", biome.minHumidity, biome.maxHumidity, error)
            || !ReadNumber(*document, "cover", biome.cover, error) || !ReadNumber(*document, "density", biome.density, error)
            || !ReadNumber(*document, "scale", biome.scale, error) || !ReadNumber(*document, "spacing", biome.spacing, error)) {
            return std::unexpected(error);
        }
        if (const auto species = document->find("species"); species != document->end()) {
            if (!species->is_array()) {
                return std::unexpected("« species » doit lister [{ \"species\": \"….espece.json\", \"share\": 50 }, ...]");
            }
            for (const Json& entry : *species) {
                VegetationBiome::Share share;
                if (!entry.is_object() || !ReadString(entry, "species", share.species, error) || !ReadNumber(entry, "share", share.share, error)
                    || share.species.empty()) {
                    return std::unexpected(error.empty() ? "espèce : { \"species\": \"….espece.json\", \"share\": 50 } attendu" : error);
                }
                if (biome.species.size() == VegetationBiome::MaxSpecies) {
                    return std::unexpected(std::format("{} espèces au plus par biome", VegetationBiome::MaxSpecies));
                }
                share.share = std::max(share.share, 0.0f);
                biome.species.push_back(std::move(share));
            }
        }
        for (float* zone : {&biome.plains, &biome.hills, &biome.mountains, &biome.cover, &biome.density}) {
            *zone = std::clamp(*zone, 0.0f, 1.0f);
        }
        biome.scale = std::clamp(biome.scale, 0.05f, 20.0f);
        biome.spacing = std::clamp(biome.spacing, 0.25f, 8.0f);
        return biome;
    }

    namespace {
        // Nombre le plus court qui se relit à l'identique.
        std::string Number(float value) {
            return std::isfinite(value) ? std::format("{}", value) : "0";
        }

        // Entre guillemets, échappé ; un UTF-8 invalide est remplacé plutôt que de lever une exception.
        std::string String(const std::string& value) {
            return Json(value).dump(-1, ' ', false, Json::error_handler_t::replace);
        }
    }

    // Écrits à la main plutôt que par Json::dump : une ligne par réglage, les listes courtes sur une ligne, comme les
    // fichiers faits à la main.
    std::string WriteTreeSpecies(const TreeSpecies& species) {
        std::string text = "{\n  \"variants\": [\n";
        for (std::size_t v = 0; v < species.variants.size(); ++v) {
            const TreeSpecies::Variant& variant = species.variants[v];
            text += std::format("    {{\n      \"model\": {},\n      \"lodModel\": {},\n      \"farModel\": {}\n    }}{}\n",
                                String(variant.model), String(variant.lodModel), String(variant.farModel),
                                v + 1 < species.variants.size() ? "," : "");
        }
        const Math::LinearColor& color = species.canopyColor;
        text += std::format("  ],\n  \"minScale\": {},\n  \"maxScale\": {},\n  \"maxSlope\": {},\n  \"tint\": {},\n"
                            "  \"trunkRadius\": {},\n  \"trunkHeight\": {},\n  \"spacing\": {},\n  \"canopyColor\": [{}, {}, {}]\n}}\n",
                            Number(species.minScale), Number(species.maxScale), Number(species.maxSlope), Number(species.tint),
                            Number(species.trunkRadius), Number(species.trunkHeight), Number(species.spacing),
                            Number(color.r), Number(color.g), Number(color.b));
        return text;
    }

    std::string WriteVegetationBiome(const VegetationBiome& biome) {
        std::string text = std::format("{{\n  \"zones\": {{\"plains\": {}, \"hills\": {}, \"mountains\": {}}},\n"
                                       "  \"altitude\": [{}, {}],\n  \"temperature\": [{}, {}],\n  \"humidity\": [{}, {}],\n"
                                       "  \"cover\": {},\n  \"density\": {},\n  \"scale\": {},\n  \"spacing\": {},\n  \"species\": [",
                                       Number(biome.plains), Number(biome.hills), Number(biome.mountains),
                                       Number(biome.minAltitude), Number(biome.maxAltitude), Number(biome.minTemperature),
                                       Number(biome.maxTemperature), Number(biome.minHumidity), Number(biome.maxHumidity),
                                       Number(biome.cover), Number(biome.density), Number(biome.scale),
                                       Number(biome.spacing));
        for (std::size_t s = 0; s < biome.species.size(); ++s) {
            text += std::format("{}\n    {{\"species\": {}, \"share\": {}}}", s == 0 ? "" : ",", String(biome.species[s].species),
                                Number(biome.species[s].share));
        }
        text += biome.species.empty() ? "]\n}\n" : "\n  ]\n}\n";
        return text;
    }
}
