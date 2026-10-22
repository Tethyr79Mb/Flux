#include "Planetology/GroundTextures.hpp"

#include "Assets/TextureCompression.hpp"

#include "Flux/Assets/Image.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <numbers>
#include <format>
#include <mutex>
#include <thread>
#include <tuple>
#include <string_view>
#include <vector>

namespace Flux::Planetology {
    namespace {
        constexpr std::array<std::string_view, static_cast<std::size_t>(GroundLayer::Count)> LayerNames{
            "roche", "herbe", "terre", "sable", "neige"};
        // Plus grand côté gardé : au-delà, l'image est réduite de moitié jusqu'à tenir (5 couches x 3 tableaux de
        // 2048² avec leurs mips : 335 Mo sans compression).
        constexpr int MaxSize = 2048;
        constexpr auto CheckInterval = std::chrono::seconds(2);
        constexpr std::string_view SettingsFile = "matieres.json";

        std::array<float, LayerNames.size()> ToRadians(const std::array<float, LayerNames.size()>& degrees) {
            std::array<float, LayerNames.size()> radians{};
            for (std::size_t i = 0; i < degrees.size(); ++i) {
                radians[i] = std::clamp(degrees[i], 0.0f, 180.0f) * (std::numbers::pi_v<float> / 180.0f);
            }
            return radians;
        }

        // L'image <nom>-<suffixe> en .png ou .jpg, dans le sous-dossier de sa matière (<nom>/), sinon à la racine du
        // dossier (rangement d'avant, encore accepté) ; chemin vide si aucune.
        std::filesystem::path FindImage(const std::filesystem::path& directory, std::string_view name, std::string_view suffix) {
            const std::filesystem::path file = Platform::PathFromUtf8(std::string(name) + "-" + std::string(suffix));
            for (const std::filesystem::path& folder : {directory / Platform::PathFromUtf8(std::string(name)), directory}) {
                for (const std::string_view extension : {".png", ".jpg", ".jpeg"}) {
                    std::filesystem::path path = folder / file;
                    path += extension;
                    std::error_code error;
                    if (std::filesystem::is_regular_file(path, error)) {
                        return path;
                    }
                }
            }
            return {};
        }

        // Moyenne de chaque carré de 2 x 2 pixels.
        void HalveImage(ImageData& image) {
            const int width = image.size.x / 2;
            const int height = image.size.y / 2;
            std::vector<std::uint8_t> half(static_cast<std::size_t>(width) * height * 4);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        const auto at = [&](int dx, int dy) {
                            return static_cast<int>(image.rgba[(static_cast<std::size_t>(2 * y + dy) * image.size.x + 2 * x + dx) * 4 + c]);
                        };
                        half[(static_cast<std::size_t>(y) * width + x) * 4 + c] =
                            static_cast<std::uint8_t>((at(0, 0) + at(1, 0) + at(0, 1) + at(1, 1) + 2) / 4);
                    }
                }
            }
            image.size = {width, height};
            image.rgba = std::move(half);
        }

        std::expected<ImageData, std::string> ReadImage(const std::filesystem::path& path) {
            auto image = LoadImageFile(path);
            if (!image) {
                return image;
            }
            if (image->size.x != image->size.y) {
                return std::unexpected(std::format("{} n'est pas carrée", Platform::ToUtf8(path.filename())));
            }
            while (image->size.x > MaxSize) {
                HalveImage(*image);
            }
            return image;
        }

        // Une image à lire : sa matière (couche des tableaux) et ce qu'elle remplit.
        enum class Channel : std::uint8_t { Color, Normal, Roughness, Occlusion };
        struct Job
        {
            std::filesystem::path path;
            std::uint32_t layer;
            Channel channel;
        };

        // Copie l'image décodée à sa place, dans la couche de sa matière : la couleur dans rouge-vert-bleu du tableau
        // de la couleur et la rugosité dans son alpha ; les normales dans rouge-vert du tableau des normales (le
        // shader refait z) et l'occlusion dans son bleu. Deux lectures par point au lieu de trois. Chaque image écrit
        // des octets différents : plusieurs fils à la fois sans verrou.
        void Place(const ImageData& image, const Job& job, GroundTextures::Decoded& decoded) {
            const std::size_t count = static_cast<std::size_t>(decoded.size) * decoded.size;
            const std::size_t first = job.layer * count * 4;
            const auto copy = [&](std::size_t target, std::size_t from, std::size_t channels) {
                std::uint8_t* out = decoded.pixels[target].data() + first + from;
                const std::uint8_t* in = image.rgba.data();
                for (std::size_t i = 0; i < count; ++i) {
                    for (std::size_t c = 0; c < channels; ++c) {
                        out[i * 4 + c] = in[i * 4 + c];
                    }
                }
            };
            switch (job.channel) {
                case Channel::Color: copy(0, 0, 3); break;
                case Channel::Roughness: copy(0, 3, 1); break; // niveaux de gris : le rouge suffit
                case Channel::Normal: copy(1, 0, 2); break;
                case Channel::Occlusion: copy(1, 2, 1); break;
            }
        }

        // Lance work sur un fil par cœur (pas plus que count - 1 en plus de celui-ci) et attend la fin.
        template <typename Work>
        void RunParallel(std::size_t count, const Work& work) {
            const std::size_t threads = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, std::max<std::size_t>(count, 1));
            std::vector<std::jthread> workers;
            for (std::size_t t = 1; t < threads; ++t) {
                workers.emplace_back(work);
            }
            work();
        }

        // Remplace les deux tableaux RGBA8 (niveau 0) par leur version BC7 avec tous les mips. Une couche d'un tableau
        // par tâche (10 en tout) ; chaque tâche écrit ses propres blocs.
        void Compress(GroundTextures::Decoded& decoded, const std::atomic<bool>& cancel) {
            const std::uint32_t size = static_cast<std::uint32_t>(decoded.size);
            const std::uint32_t levels = MipLevelCount(size);
            const std::uint32_t layers = static_cast<std::uint32_t>(LayerNames.size());
            std::vector<std::size_t> levelOffsets(levels + 1, 0);
            for (std::uint32_t level = 0; level < levels; ++level) {
                const std::uint32_t s = std::max(size >> level, 1u);
                levelOffsets[level + 1] = levelOffsets[level] + BC7Size(s, s) * layers;
            }
            std::array<std::vector<std::uint8_t>, 2> compressed;
            for (auto& pixels : compressed) {
                pixels.resize(levelOffsets.back());
            }
            const std::size_t layerBytes = static_cast<std::size_t>(size) * size * 4;
            std::atomic<std::size_t> next = 0;
            RunParallel(2 * layers, [&] {
                for (std::size_t job = next++; job < 2 * layers && !cancel; job = next++) {
                    const std::size_t texture = job / layers;
                    const std::uint32_t layer = static_cast<std::uint32_t>(job % layers);
                    const std::span<const std::uint8_t> source(decoded.pixels[texture].data() + layer * layerBytes, layerBytes);
                    const std::vector<std::uint8_t> mips = BuildMips(source, size, texture == 0);
                    std::size_t at = 0;
                    for (std::uint32_t level = 0; level < levels; ++level) {
                        const std::uint32_t s = std::max(size >> level, 1u);
                        const std::size_t bytes = BC7Size(s, s);
                        CompressBC7(std::span(mips).subspan(at, static_cast<std::size_t>(s) * s * 4), s, s, texture == 0,
                                    std::span(compressed[texture]).subspan(levelOffsets[level] + layer * bytes, bytes));
                        at += static_cast<std::size_t>(s) * s * 4;
                    }
                }
            });
            decoded.pixels = std::move(compressed);
            decoded.compressed = true;
        }

        // Cache des textures compressées (bin/Cache/sol-bc7.bin) : la compression prend plusieurs secondes, elle n'est
        // refaite que si une image change. Clé : chemin, taille et date de chaque image, et ce qui change le résultat.
        constexpr std::uint64_t CacheMagic = 0x3743424C4F535846; // « FXSOLBC7 »
        constexpr std::uint32_t CacheVersion = 1; // à changer avec la compression, les mips ou la disposition

        struct CacheHeader
        {
            std::uint64_t magic = CacheMagic;
            std::uint32_t version = CacheVersion;
            std::uint32_t size = 0;
            std::uint64_t key = 0;
            std::uint64_t bytes[2]{};
        };

        std::uint64_t CacheKey(const std::vector<Job>& jobs) {
            std::uint64_t hash = 14695981039346656037ull; // FNV-1a
            const auto add = [&](const void* data, std::size_t size) {
                for (std::size_t i = 0; i < size; ++i) {
                    hash = (hash ^ static_cast<const std::uint8_t*>(data)[i]) * 1099511628211ull;
                }
            };
            add(&MaxSize, sizeof(MaxSize));
            for (const Job& job : jobs) {
                // Chemin entier : une image déplacée d'un dossier à l'autre refait le cache.
                const std::string name = Platform::ToUtf8(job.path);
                add(name.data(), name.size());
                std::error_code error;
                const std::uint64_t bytes = std::filesystem::file_size(job.path, error);
                const auto time = std::filesystem::last_write_time(job.path, error).time_since_epoch().count();
                add(&bytes, sizeof(bytes));
                add(&time, sizeof(time));
            }
            return hash;
        }

        bool ReadCache(const std::filesystem::path& path, std::uint64_t key, GroundTextures::Decoded& decoded) {
            std::vector<std::uint8_t> bytes;
            CacheHeader header;
            if (!Platform::ReadFileBytes(path, bytes) || bytes.size() < sizeof(header)) {
                return false;
            }
            std::memcpy(&header, bytes.data(), sizeof(header));
            if (header.magic != CacheMagic || header.version != CacheVersion || header.key != key
                || bytes.size() != sizeof(header) + header.bytes[0] + header.bytes[1]) {
                return false;
            }
            decoded.size = static_cast<int>(header.size);
            decoded.compressed = true;
            const auto first = bytes.begin() + sizeof(header);
            decoded.pixels[0].assign(first, first + static_cast<std::ptrdiff_t>(header.bytes[0]));
            decoded.pixels[1].assign(first + static_cast<std::ptrdiff_t>(header.bytes[0]), bytes.end());
            return true;
        }

        // Écrit à côté puis renomme : un jeu fermé en pleine écriture ne laisse pas de cache tronqué qui passerait pour bon.
        void WriteCache(const std::filesystem::path& path, std::uint64_t key, const GroundTextures::Decoded& decoded) {
            CacheHeader header;
            header.size = static_cast<std::uint32_t>(decoded.size);
            header.key = key;
            header.bytes[0] = decoded.pixels[0].size();
            header.bytes[1] = decoded.pixels[1].size();
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            std::filesystem::path temporary = path;
            temporary += ".tmp";
            {
                std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(&header), sizeof(header));
                for (const auto& pixels : decoded.pixels) {
                    file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
                }
                if (!file) {
                    Log::Warning("Sol des planètes : cache {} non écrit", Platform::ToUtf8(path));
                    return;
                }
            }
            std::filesystem::rename(temporary, path, error);
            if (error) {
                Log::Warning("Sol des planètes : cache {} non écrit ({})", Platform::ToUtf8(path), error.message());
            }
        }

        // Lit et range toutes les images du dossier. La première (roche-couleur) donne la taille, les autres sont
        // décodées en parallèle, directement à leur place ; puis compressées si compress.
        GroundTextures::Decoded Decode(const std::filesystem::path& directory, const std::filesystem::path& cachePath,
                                       const std::atomic<bool>& cancel) {
            const bool compress = !cachePath.empty();
            GroundTextures::Decoded decoded;
            std::vector<Job> jobs;
            for (std::uint32_t layer = 0; layer < LayerNames.size(); ++layer) {
                for (const auto& [suffix, channel, required] : {std::tuple{"couleur", Channel::Color, true},
                                                               std::tuple{"normales", Channel::Normal, true},
                                                               std::tuple{"rugosite", Channel::Roughness, true},
                                                               std::tuple{"occlusion", Channel::Occlusion, false}}) {
                    std::filesystem::path path = FindImage(directory, LayerNames[layer], suffix);
                    if (!path.empty()) {
                        jobs.push_back({std::move(path), layer, channel});
                    } else if (required) {
                        decoded.error = std::format("image {}-{} absente de {}", LayerNames[layer], suffix, Platform::ToUtf8(directory));
                        return decoded;
                    }
                }
            }

            const std::uint64_t key = compress ? CacheKey(jobs) : 0;
            if (compress && ReadCache(cachePath, key, decoded)) {
                return decoded;
            }

            auto first = ReadImage(jobs.front().path);
            if (!first) {
                decoded.error = first.error();
                return decoded;
            }
            decoded.size = first->size.x;
            const std::size_t bytes = static_cast<std::size_t>(decoded.size) * decoded.size * 4 * LayerNames.size();
            decoded.pixels[0].resize(bytes);
            // Sans image d'occlusion : 255 (aucune).
            decoded.pixels[1].resize(bytes);
            for (std::size_t i = 0; i < bytes; i += 4) {
                decoded.pixels[1][i + 2] = 255;
                decoded.pixels[1][i + 3] = 255;
            }
            Place(*first, jobs.front(), decoded);
            first->rgba = {};

            std::atomic<std::size_t> next = 1;
            std::mutex errorMutex;
            const auto work = [&] {
                for (std::size_t j = next++; j < jobs.size() && !cancel; j = next++) {
                    auto image = ReadImage(jobs[j].path);
                    if (image && image->size.x != decoded.size) {
                        image = std::unexpected(std::format("{} : toutes les images doivent avoir la même taille",
                                                            Platform::ToUtf8(jobs[j].path.filename())));
                    }
                    if (!image) {
                        const std::scoped_lock lock(errorMutex);
                        decoded.error = image.error();
                        continue;
                    }
                    Place(*image, jobs[j], decoded);
                }
            };
            // Un fil par cœur (pas plus que d'images) ; chaque image décodée prend 16 Mo le temps d'être rangée.
            RunParallel(jobs.size() - 1, work);
            if (compress && !cancel && decoded.error.empty()) {
                const auto start = std::chrono::steady_clock::now();
                Compress(decoded, cancel);
                if (!cancel) {
                    WriteCache(cachePath, key, decoded);
                }
                Log::Info("Sol des planètes : textures compressées en BC7 en {:.1f} s",
                          std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            }
            if (cancel) {
                decoded.error = "annulé";
            }
            return decoded;
        }
    }

    void GroundTextures::ReadSettings() {
        std::array<float, LayerNames.size()> degrees = DefaultMaxAngles;
        const std::filesystem::path path = m_directory / Platform::PathFromUtf8(std::string(SettingsFile));
        std::vector<std::uint8_t> bytes;
        if (Platform::ReadFileBytes(path, bytes)) {
            // Fichier faux : les valeurs par défaut, et un message ; une matière inconnue est signalée (faute de frappe).
            const nlohmann::json document = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
            const auto rotation = document.is_object() ? document.find("rotation") : document.end();
            if (document.is_discarded() || !document.is_object() || rotation == document.end() || !rotation->is_object()) {
                Log::Error("Sol des planètes : {} illisible (attendu : {{\"rotation\": {{\"sable\": 10, ...}}}})", Platform::ToUtf8(path));
            } else {
                for (const auto& [name, value] : rotation->items()) {
                    const auto layer = std::ranges::find(LayerNames, name);
                    if (layer == LayerNames.end() || !value.is_number()) {
                        Log::Error("Sol des planètes : {} : « {} » ignoré (matières : roche, herbe, terre, sable, neige ; angle en degrés)",
                                   Platform::ToUtf8(path.filename()), name);
                        continue;
                    }
                    degrees[static_cast<std::size_t>(layer - LayerNames.begin())] = value.get<float>();
                }
            }
        }
        m_maxAngles = ToRadians(degrees);
    }

    GroundTextures::~GroundTextures() {
        if (m_cancel) {
            *m_cancel = true; // fermeture : les images restantes ne sont pas décodées (le futur attend la fin)
        }
    }

    const RHI::BindGroup* GroundTextures::Get(RHI::Device& device, const ModelCache& models,
                                              const std::filesystem::path& assetsDirectory) {
        // Décodage fini : envoyé au GPU (le seul moment où le fil principal attend, le temps de la copie).
        if (m_decoding.valid() && m_decoding.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            Decoded decoded = m_decoding.get();
            m_cancel.reset();
            if (!decoded.error.empty()) {
                Log::Error("Sol des planètes : {}", decoded.error);
            } else {
                device.WaitIdle(); // l'ancien groupe peut servir à une image en vol
                m_group.reset();
                if (!Upload(device, models, decoded)) {
                    m_group.reset();
                }
            }
        }

        const std::filesystem::path directory = assetsDirectory / "Textures" / "Sol";
        const auto now = std::chrono::steady_clock::now();
        if (directory != m_directory) {
            m_directory = directory;
            m_writeTime = std::filesystem::file_time_type::min();
            m_checked = {};
            m_settingsTime = std::filesystem::file_time_type::max(); // relu à la vérification ci-dessous
        }
        // Un décodage à la fois : les changements arrivés pendant celui-ci sont vus à la vérification suivante.
        if (now - m_checked >= CheckInterval && !m_decoding.valid()) {
            m_checked = now;
            std::error_code error;
            const std::filesystem::path settings = m_directory / Platform::PathFromUtf8(std::string(SettingsFile));
            std::filesystem::file_time_type settingsTime = std::filesystem::last_write_time(settings, error);
            if (error) {
                settingsTime = std::filesystem::file_time_type::min();
            }
            if (settingsTime != m_settingsTime) {
                m_settingsTime = settingsTime;
                ReadSettings();
            }
            const std::filesystem::file_time_type writeTime = NewestWriteTime();
            if (writeTime != m_writeTime) {
                m_writeTime = writeTime;
                if (writeTime == std::filesystem::file_time_type::min()) {
                    device.WaitIdle(); // plus d'images : couleurs unies
                    m_group.reset();
                    for (auto& texture : m_textures) {
                        texture.reset();
                    }
                } else {
                    m_cancel = std::make_shared<std::atomic<bool>>(false);
                    // Sans formats BC : pas de cache, les images en RGBA8.
                    std::filesystem::path cache;
                    if (device.HasTextureCompressionBC()) {
                        cache = Platform::GetExecutableDirectory() / "Cache" / "sol-bc7.bin";
                    }
                    m_decoding = std::async(std::launch::async, [directory, cache, cancel = m_cancel] {
                        return Decode(directory, cache, *cancel);
                    });
                }
            }
        }
        return m_group.get();
    }

    // La plus récente des images du dossier (aucune : la plus petite valeur ; sous Windows, une date peut être avant
    // l'origine de l'horloge des fichiers).
    std::filesystem::file_time_type GroundTextures::NewestWriteTime() const {
        std::filesystem::file_time_type newest = std::filesystem::file_time_type::min();
        for (const std::string_view name : LayerNames) {
            for (const std::string_view suffix : {"couleur", "normales", "rugosite", "occlusion"}) {
                if (const std::filesystem::path path = FindImage(m_directory, name, suffix); !path.empty()) {
                    std::error_code error;
                    const auto time = std::filesystem::last_write_time(path, error);
                    if (!error && time > newest) {
                        newest = time;
                    }
                }
            }
        }
        return newest;
    }

    bool GroundTextures::Upload(RHI::Device& device, const ModelCache& models, Decoded& decoded) {
        const auto fail = [](std::string_view message) {
            Log::Error("Sol des planètes : {}", message);
            return false;
        };
        const int size = decoded.size;
        std::array<std::vector<std::uint8_t>, 2>& pixels = decoded.pixels;
        const RHI::Extent extent{static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(size)};
        // Alpha d'une image sRGB (rugosité) : linéaire, en RGBA8 comme en BC7.
        constexpr std::array formats{RHI::Format::RGBA8Srgb, RHI::Format::RGBA8Unorm};
        constexpr std::array compressedFormats{RHI::Format::BC7Srgb, RHI::Format::BC7Unorm};
        const auto layers = static_cast<std::uint32_t>(LayerNames.size());
        for (std::size_t i = 0; i < m_textures.size(); ++i) {
            auto texture = decoded.compressed
                               ? device.CreateTextureArrayLevels(compressedFormats[i], extent, layers, MipLevelCount(extent.width), pixels[i])
                               : device.CreateTextureArray(formats[i], extent, layers, pixels[i], true);
            if (!texture) {
                return fail(texture.error());
            }
            m_textures[i] = std::move(*texture);
        }
        if (!m_sampler) {
            auto sampler = device.CreateSampler({.filter = RHI::Filter::Linear, .mipFilter = RHI::Filter::Linear,
                                                 .address = RHI::AddressMode::Repeat, .anisotropy = 16.0f});
            if (!sampler) {
                return fail(sampler.error());
            }
            m_sampler = std::move(*sampler);
        }
        m_group = device.CreateBindGroup(models.GetGroundLayout());
        if (!m_group) {
            return fail("plus de groupe libre pour les textures");
        }
        m_group->SetTexture(ModelCache::GroundColorSlot, *m_textures[0], *m_sampler);
        m_group->SetTexture(ModelCache::GroundNormalSlot, *m_textures[1], *m_sampler);
        Log::Info("Sol des planètes : {} matières en {}x{}{}", LayerNames.size(), size, size, decoded.compressed ? " (BC7)" : "");
        return true;
    }
}
