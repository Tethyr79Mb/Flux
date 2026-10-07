#include "Renderer/GroundTextures.hpp"

#include "Flux/Assets/Image.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <format>
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
    }

    const RHI::BindGroup* GroundTextures::Get(RHI::Device& device, const ModelCache& models,
                                              const std::filesystem::path& assetsDirectory) {
        const std::filesystem::path directory = assetsDirectory / "Textures" / "Sol";
        const auto now = std::chrono::steady_clock::now();
        if (directory != m_directory) {
            m_directory = directory;
            m_writeTime = std::filesystem::file_time_type::min();
            m_checked = {};
        }
        if (now - m_checked >= CheckInterval) {
            m_checked = now;
            const std::filesystem::file_time_type writeTime = NewestWriteTime();
            if (writeTime != m_writeTime) {
                m_writeTime = writeTime;
                device.WaitIdle(); // l'ancien groupe peut servir à une image en vol
                m_group.reset();
                for (auto& texture : m_textures) {
                    texture.reset();
                }
                if (writeTime != std::filesystem::file_time_type::min() && !Load(device, models)) {
                    m_group.reset();
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

    bool GroundTextures::Load(RHI::Device& device, const ModelCache& models) {
        // Tableaux : couleur (sRGB) ; normales ; occlusion (rouge) et rugosité (vert).
        std::array<std::vector<std::uint8_t>, 3> pixels;
        int size = 0;
        const auto fail = [](std::string_view message) {
            Log::Error("Sol des planètes : {}", message);
            return false;
        };
        for (const std::string_view name : LayerNames) {
            const auto read = [&](std::string_view suffix, bool required) -> std::expected<ImageData, std::string> {
                const std::filesystem::path path = FindImage(m_directory, name, suffix);
                if (path.empty()) {
                    if (required) {
                        return std::unexpected(std::format("image {}-{} absente de {}", name, suffix, Platform::ToUtf8(m_directory)));
                    }
                    return ImageData{};
                }
                auto image = ReadImage(path);
                if (image && size != 0 && image->size.x != size) {
                    return std::unexpected(std::format("{}-{} : toutes les images doivent avoir la même taille", name, suffix));
                }
                return image;
            };
            auto color = read("couleur", true);
            if (!color) {
                return fail(color.error());
            }
            size = color->size.x;
            auto normal = read("normales", true);
            auto roughness = read("rugosite", true);
            auto occlusion = read("occlusion", false);
            for (const auto* image : {&normal, &roughness, &occlusion}) {
                if (!*image) {
                    return fail(image->error());
                }
            }
            pixels[0].insert(pixels[0].end(), color->rgba.begin(), color->rgba.end());
            pixels[1].insert(pixels[1].end(), normal->rgba.begin(), normal->rgba.end());
            const std::size_t count = static_cast<std::size_t>(size) * size;
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint8_t ambient = occlusion->rgba.empty() ? 255 : occlusion->rgba[i * 4];
                pixels[2].insert(pixels[2].end(), {ambient, roughness->rgba[i * 4], 0, 255});
            }
        }

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
        m_group = device.CreateBindGroup(models.GetMaterialLayout());
        if (!m_group) {
            return fail("plus de groupe libre pour les textures");
        }
        // Les images d'un matériau glTF (lues par l'ombre des tuiles : seuil d'alpha) : blanches.
        for (std::uint32_t slot = 0; slot < ModelCache::GroundColorSlot; ++slot) {
            m_group->SetTexture(slot, models.GetWhiteImage(), models.GetSampler());
        }
        m_group->SetTexture(ModelCache::GroundColorSlot, *m_textures[0], *m_sampler);
        m_group->SetTexture(ModelCache::GroundNormalSlot, *m_textures[1], *m_sampler);
        m_group->SetTexture(ModelCache::GroundRoughnessSlot, *m_textures[2], *m_sampler);
        Log::Info("Sol des planètes : {} matières en {}x{}", LayerNames.size(), size, size);
        return true;
    }
}
