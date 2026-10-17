#include "Renderer/GroundTextures.hpp"

#include "Flux/Assets/Image.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <numbers>
#include <format>
#include <mutex>
#include <thread>
#include <tuple>
#include <string_view>
#include <vector>

namespace Flux {
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

        // L'image <nom>-<suffixe> en .png ou .jpg, chemin vide si aucune.
        std::filesystem::path FindImage(const std::filesystem::path& directory, std::string_view name, std::string_view suffix) {
            for (const std::string_view extension : {".png", ".jpg", ".jpeg"}) {
                std::filesystem::path path = directory / Platform::PathFromUtf8(std::string(name) + "-" + std::string(suffix) + std::string(extension));
                std::error_code error;
                if (std::filesystem::is_regular_file(path, error)) {
                    return path;
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

        // Copie l'image décodée à sa place : sa couche du tableau de la couleur ou des normales, ou un canal du
        // tableau occlusion-rugosité. Chaque image écrit des octets différents : plusieurs fils à la fois sans verrou.
        void Place(const ImageData& image, const Job& job, GroundTextures::Decoded& decoded) {
            const std::size_t count = static_cast<std::size_t>(decoded.size) * decoded.size;
            const std::size_t first = job.layer * count * 4;
            switch (job.channel) {
                case Channel::Color:
                case Channel::Normal: {
                    std::vector<std::uint8_t>& pixels = decoded.pixels[job.channel == Channel::Color ? 0 : 1];
                    std::copy(image.rgba.begin(), image.rgba.end(), pixels.begin() + static_cast<std::ptrdiff_t>(first));
                    break;
                }
                case Channel::Roughness:
                case Channel::Occlusion: {
                    std::uint8_t* out = decoded.pixels[2].data() + first + (job.channel == Channel::Occlusion ? 0 : 1);
                    const std::uint8_t* in = image.rgba.data();
                    for (std::size_t i = 0; i < count; ++i) {
                        out[i * 4] = in[i * 4];
                    }
                    break;
                }
            }
        }

        // Lit et range toutes les images du dossier. La première (roche-couleur) donne la taille, les autres sont
        // décodées en parallèle, directement à leur place.
        GroundTextures::Decoded Decode(const std::filesystem::path& directory, const std::atomic<bool>& cancel) {
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

            auto first = ReadImage(jobs.front().path);
            if (!first) {
                decoded.error = first.error();
                return decoded;
            }
            decoded.size = first->size.x;
            const std::size_t bytes = static_cast<std::size_t>(decoded.size) * decoded.size * 4 * LayerNames.size();
            decoded.pixels[0].resize(bytes);
            decoded.pixels[1].resize(bytes);
            // Sans image d'occlusion : 255 (aucune).
            decoded.pixels[2].resize(bytes);
            for (std::size_t i = 0; i < bytes; i += 4) {
                decoded.pixels[2][i] = 255;
                decoded.pixels[2][i + 3] = 255;
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
            const std::size_t threads = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, jobs.size() - 1);
            {
                std::vector<std::jthread> workers;
                for (std::size_t t = 1; t < threads; ++t) {
                    workers.emplace_back(work);
                }
                work();
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
                    m_decoding = std::async(std::launch::async, [directory, cancel = m_cancel] { return Decode(directory, *cancel); });
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
        std::array<std::vector<std::uint8_t>, 3>& pixels = decoded.pixels;
        const RHI::Extent extent{static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(size)};
        constexpr std::array formats{RHI::Format::RGBA8Srgb, RHI::Format::RGBA8Unorm, RHI::Format::RGBA8Unorm};
        for (std::size_t i = 0; i < m_textures.size(); ++i) {
            auto texture = device.CreateTextureArray(formats[i], extent, static_cast<std::uint32_t>(LayerNames.size()), pixels[i], true);
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
        m_group->SetTexture(ModelCache::GroundRoughnessSlot, *m_textures[2], *m_sampler);
        Log::Info("Sol des planètes : {} matières en {}x{}", LayerNames.size(), size, size);
        return true;
    }
}
