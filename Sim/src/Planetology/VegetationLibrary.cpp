#include "Planetology/VegetationLibrary.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <numeric>

namespace Flux::Planetology {
    namespace {
        constexpr auto CheckInterval = std::chrono::seconds(1);

        // Aperçus de l'éditeur, communs à toutes les bibliothèques (une par moteur de rendu). Le compteur se lit sans
        // verrou à chaque Get ; le verrou ne sert qu'aux relectures.
        std::mutex s_previewMutex;
        std::map<std::string, std::string, std::less<>> s_previews;
        std::atomic<std::uint64_t> s_previewRevision{0};
    }

    void SetVegetationPreview(std::string_view path, std::string text) {
        {
            const std::scoped_lock lock(s_previewMutex);
            if (text.empty()) {
                const auto found = s_previews.find(path);
                if (found == s_previews.end()) {
                    return;
                }
                s_previews.erase(found);
            } else {
                s_previews.insert_or_assign(std::string(path), std::move(text));
            }
        }
        ++s_previewRevision;
    }

    std::optional<std::string> FindVegetationPreview(std::string_view path) {
        const std::scoped_lock lock(s_previewMutex);
        const auto found = s_previews.find(path);
        return found == s_previews.end() ? std::nullopt : std::optional<std::string>(found->second);
    }

    std::uint64_t GetVegetationPreviewRevision() {
        return s_previewRevision.load(std::memory_order_relaxed);
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
        } else if (entry->previews != GetVegetationPreviewRevision()) {
            rebuild = true; // réglage en cours dans l'éditeur : suivi image par image
        } else if (now - entry->checked >= CheckInterval) {
            entry->checked = now;
            rebuild = Changed(*entry);
        }
        entry->lastUsed = m_calls;
        if (rebuild) {
            entry->checked = now;
            entry->previews = GetVegetationPreviewRevision();
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
            if (std::optional<std::string> preview = FindVegetationPreview(path)) {
                text = std::move(*preview);
                return true;
            }
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
            // Espèces d'une liste du biome : leur première variante | nombre << 8 et leurs parts cumulées (la
            // dernière à 1) ; total : la somme des parts (0 : aucune espèce lue). canopy : couleur moyenne des cimes.
            struct Shares
            {
                std::array<std::uint32_t, 4> entries{};
                std::array<float, 4> cumulative{1.0f, 1.0f, 1.0f, 1.0f};
                float total = 0.0f;
            };
            const auto gather = [&](const std::vector<VegetationBiome::Share>& list, Math::LinearColor* canopy) {
                Shares shares;
                std::array<float, 4> weights{};
                std::uint32_t used = 0;
                for (const VegetationBiome::Share& share : list) {
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
                            set.species.push_back({variant, species.minScale, species.maxScale, species.maxSlope, species.tint,
                                                   species.trunkRadius, species.trunkHeight, species.spacing});
                        }
                    }
                    shares.entries[used] = first->second | (static_cast<std::uint32_t>(species.variants.size()) << 8);
                    weights[used] = share.share;
                    if (canopy) {
                        canopy->r += species.canopyColor.r * share.share;
                        canopy->g += species.canopyColor.g * share.share;
                        canopy->b += species.canopyColor.b * share.share;
                    }
                    ++used;
                }
                shares.total = std::accumulate(weights.begin(), weights.end(), 0.0f);
                if (shares.total > 0.0f) {
                    float sum = 0.0f;
                    for (std::uint32_t s = 0; s < used; ++s) {
                        sum += weights[s];
                        shares.cumulative[s] = s + 1 == used ? 1.0f : sum / shares.total;
                    }
                }
                for (std::uint32_t s = used; s < 4; ++s) {
                    shares.entries[s] = used > 0 ? shares.entries[used - 1] : 0u;
                }
                return shares;
            };
            Math::LinearColor canopy{0.0f, 0.0f, 0.0f, 1.0f};
            const Shares trees = gather(biome->species, &canopy);
            const Shares understory = gather(biome->understory, nullptr);
            // Sans espèce : le biome prend sa place (un désert), sans arbres.
            const float density = trees.total > 0.0f ? biome->density : 0.0f;
            const float understoryDensity = understory.total > 0.0f ? biome->understoryDensity : 0.0f;
            if (trees.total > 0.0f) {
                canopy = {canopy.r / trees.total, canopy.g / trees.total, canopy.b / trees.total, 1.0f};
            }
            set.biomes.push_back({{biome->plains, biome->hills, biome->mountains, biome->cover},
                                    {biome->minAltitude * 0.01f, biome->maxAltitude * 0.01f, biome->minTemperature, biome->maxTemperature},
                                    {biome->minHumidity, biome->maxHumidity, density, biome->scale},
                                    {canopy.r, canopy.g, canopy.b, biome->grove},
                                    {biome->granite, biome->limestone, biome->sandstone, biome->basalt},
                                    // Bornes ouvertes au-delà de 0 et 1 : la bande fondue (BiomeFit) ne rogne pas un sol à nu ou épais.
                                    {biome->minSoil > 0.0f ? biome->minSoil : -1.0f, biome->maxSoil < 1.0f ? biome->maxSoil : 2.0f,
                                     understoryDensity, biome->understoryScale}});
            set.biomeSpecies.push_back(trees.entries);
            set.biomeShares.push_back(trees.cumulative);
            set.understorySpecies.push_back(understory.entries);
            set.understoryShares.push_back(understory.cumulative);
            set.biomeSpacing.push_back(biome->spacing);
        }
    }
}
