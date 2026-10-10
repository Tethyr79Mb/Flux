#include "Flux/Assets/Image.hpp"

#include "Flux/Platform/Filesystem.hpp"

#include <cstring>
#include <limits>
#include <memory>

// Déclarations seules : l'implémentation est dans StbImage.c.
#define STBI_NO_STDIO
#include <stb_image.h>

namespace Flux {
    std::expected<ImageData, std::string> LoadImageMemory(std::span<const std::uint8_t> bytes) {
        if (bytes.empty()) {
            return std::unexpected("fichier vide");
        }
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return std::unexpected("fichier trop gros");
        }
        const int length = static_cast<int>(bytes.size());
        int width = 0;
        int height = 0;
        int channels = 0;
        // Taille lue avant de décoder : une image géante est refusée sans allouer sa mémoire.
        if (!stbi_info_from_memory(bytes.data(), length, &width, &height, &channels)) {
            return std::unexpected(std::string("format non reconnu (") + stbi_failure_reason() + ")");
        }
        if (width <= 0 || height <= 0 || width > MaxImageSize || height > MaxImageSize) {
            return std::unexpected("taille " + std::to_string(width) + " x " + std::to_string(height)
                                   + " non gérée (" + std::to_string(MaxImageSize) + " pixels de côté au plus)");
        }
        const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
            stbi_load_from_memory(bytes.data(), length, &width, &height, &channels, 4), &stbi_image_free);
        if (!pixels) {
            return std::unexpected(std::string("décodage impossible (") + stbi_failure_reason() + ")");
        }
        ImageData image;
        image.size = {width, height};
        image.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        std::memcpy(image.rgba.data(), pixels.get(), image.rgba.size());
        return image;
    }

    std::expected<ImageData, std::string> LoadImageFile(const std::filesystem::path& path) {
        std::vector<std::uint8_t> bytes;
        if (!Platform::ReadFileBytes(path, bytes)) {
            return std::unexpected("lecture de « " + Platform::ToUtf8(path.filename()) + " » impossible");
        }
        auto image = LoadImageMemory(bytes);
        if (!image) {
            return std::unexpected("« " + Platform::ToUtf8(path.filename()) + " » : " + image.error());
        }
        return image;
    }
}
