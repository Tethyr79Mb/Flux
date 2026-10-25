#include "Planetology/VegetationLibrary.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <numeric>

namespace Flux::Planetology {
    namespace {
        constexpr auto CheckInterval = std::chrono::seconds(1);

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
            || !ReadNumber(*document, "maxSlope", species.maxSlope, error) || !ReadNumber(*document, "tint", species.tint, error)) {
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
            || !ReadNumber(*document, "scale", biome.scale, error)) {
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
                if (biome.species.size() == VegetationSet::MaxSharesPerBiome) {
                    return std::unexpected(std::format("{} espèces au plus par biome", VegetationSet::MaxSharesPerBiome));
                }
                share.share = std::max(share.share, 0.0f);
                biome.species.push_back(std::move(share));
            }
        }
        for (float* zone : {&biome.plains, &biome.hills, &biome.mountains, &biome.cover, &biome.density}) {
            *zone = std::clamp(*zone, 0.0f, 1.0f);
        }
        biome.scale = std::clamp(biome.scale, 0.05f, 20.0f);
        return biome;
    }

    const VegetationSet& VegetationLibrary::Get(const std::filesystem::path& assetsDirectory, const VegetationComponent& vegetation) {
        constexpr std::size_t MaxEntries = 8;
        const std::array<const std::string*, VegetationSet::MaxBiomes> paths{&vegetation.biome1, &vegetation.biome2, &vegetation.biome3,
                                                                             &vegetation.biome4, &vegetation.biome5, &vegetation.biome6,
                                                                             &vegetation.biome7, &vegetation.biome8};
        ++m_calls;
        const auto now = std::chrono::steady_clock::now();
        auto entry = std::ranges::find_if(m_entries, [&](const Entry& e) {
            return e.directory == assetsDirectory && std::ranges::equal(e.paths, paths, {}, {}, [](const std::string* p) -> const std::string& { return *p; });
        });
        bool rebuild = false;
        if (entry == m_entries.end()) {
            if (m_entries.size() == MaxEntries) {
                m_entries.erase(std::ranges::min_element(m_entries, {}, &Entry::lastUsed));
            }
            Entry created;
            created.directory = assetsDirectory;
            for (std::size_t b = 0; b < paths.size(); ++b) {
                created.paths[b] = *paths[b];
            }
            m_entries.push_back(std::move(created));
            entry = m_entries.end() - 1;
            rebuild = true;
        } else if (now - entry->checked >= CheckInterval) {
            entry->checked = now;
            rebuild = Changed(*entry);
        }
        entry->lastUsed = m_calls;
        if (rebuild) {
            entry->checked = now;
            Rebuild(*entry);
            entry->set.revision = ++m_revision;
        }
        return entry->set;
    }

    bool VegetationLibrary::Changed(const Entry& entry) {
        for (const auto& [path, time] : entry.files) {
            std::error_code error;
            auto current = std::filesystem::last_write_time(entry.directory / Platform::PathFromUtf8(path), error);
            if (error) {
                current = std::filesystem::file_time_type::min();
            }
            if (current != time) {
                return true;
            }
        }
        return false;
    }

    void VegetationLibrary::Rebuild(Entry& entry) {
        VegetationSet& set = entry.set;
        set = {};
        entry.files.clear();
        // Lit un fichier et note sa date (min : absent, relu s'il apparaît).
        const auto read = [&entry](const std::string& path, std::string& text) {
            const std::filesystem::path full = entry.directory / Platform::PathFromUtf8(path);
            std::error_code error;
            auto time = std::filesystem::last_write_time(full, error);
            entry.files[path] = error ? std::filesystem::file_time_type::min() : time;
            std::vector<std::uint8_t> bytes;
            if (!Platform::ReadFileBytes(full, bytes)) {
                Log::Error("Végétation : {} introuvable", path);
                return false;
            }
            text.assign(bytes.begin(), bytes.end());
            return true;
        };
        // Espèces déjà mises dans la table : leur première variante et leur nombre.
        std::map<std::string, std::uint32_t> placed;
        std::map<std::string, TreeSpecies> speciesFiles;
        for (const std::string& path : entry.paths) {
            if (path.empty()) {
                continue;
            }
            std::string text;
            if (!read(path, text)) {
                continue;
            }
            auto biome = ParseVegetationBiome(text);
            if (!biome) {
                Log::Error("Végétation : {} : {}", path, biome.error());
                continue;
            }
            std::array<std::uint32_t, 4> entries{};
            std::array<float, 4> weights{};
            Math::LinearColor canopy{0.0f, 0.0f, 0.0f, 1.0f};
            std::uint32_t used = 0;
            for (const VegetationBiome::Share& share : biome->species) {
                auto found = speciesFiles.find(share.species);
                if (found == speciesFiles.end()) {
                    std::string speciesText;
                    if (!read(share.species, speciesText)) {
                        continue;
                    }
                    auto species = ParseTreeSpecies(speciesText);
                    if (!species) {
                        Log::Error("Végétation : {} : {}", share.species, species.error());
                        continue;
                    }
                    found = speciesFiles.emplace(share.species, std::move(*species)).first;
                }
                const TreeSpecies& species = found->second;
                auto first = placed.find(share.species);
                if (first == placed.end()) {
                    if (set.species.size() + species.variants.size() > VegetationSet::MaxSpecies) {
                        Log::Error("Végétation : {} variantes d'arbres au plus par planète, {} ignorée", VegetationSet::MaxSpecies, share.species);
                        continue;
                    }
                    first = placed.emplace(share.species, static_cast<std::uint32_t>(set.species.size())).first;
                    for (const TreeSpecies::Variant& variant : species.variants) {
                        set.species.push_back({variant, species.minScale, species.maxScale, species.maxSlope, species.tint});
                    }
                }
                entries[used] = first->second | (static_cast<std::uint32_t>(species.variants.size()) << 8);
                weights[used] = share.share;
                canopy.r += species.canopyColor.r * share.share;
                canopy.g += species.canopyColor.g * share.share;
                canopy.b += species.canopyColor.b * share.share;
                ++used;
            }
            const float total = std::accumulate(weights.begin(), weights.end(), 0.0f);
            // Sans espèce : le biome prend sa place (un désert), sans arbres.
            const float density = total > 0.0f ? biome->density : 0.0f;
            std::array<float, 4> cumulative{1.0f, 1.0f, 1.0f, 1.0f};
            if (total > 0.0f) {
                float sum = 0.0f;
                for (std::uint32_t s = 0; s < used; ++s) {
                    sum += weights[s];
                    cumulative[s] = s + 1 == used ? 1.0f : sum / total;
                }
                canopy = {canopy.r / total, canopy.g / total, canopy.b / total, 1.0f};
            }
            for (std::uint32_t s = used; s < 4; ++s) {
                entries[s] = used > 0 ? entries[used - 1] : 0u;
            }
            set.biomes.push_back({{biome->plains, biome->hills, biome->mountains, biome->cover},
                                    {biome->minAltitude * 0.01f, biome->maxAltitude * 0.01f, biome->minTemperature, biome->maxTemperature},
                                    {biome->minHumidity, biome->maxHumidity, density, biome->scale},
                                    {canopy.r, canopy.g, canopy.b, 0.0f}});
            set.biomeSpecies.push_back(entries);
            set.biomeShares.push_back(cumulative);
        }
    }
}
