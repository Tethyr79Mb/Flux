#pragma once

#include "Renderer/Vulkan/Vulkan.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace Flux::Vk {
    class Device;

    // Façon de lire une image à plusieurs couches.
    enum class ImageLayers : std::uint8_t {
        Single, // image 2D
        Cube,   // 6 faces, lue par direction
        Array,  // tableau d'images 2D de même taille, lues par indice (Sampler2DArray)
    };

    // Image 2D (ou cube : 6 faces, ou tableau) allouée par VMA, avec sa vue (tous les niveaux de mip) : texture ou attachment de rendu.
    class Image {
        public:
        // Image vierge (contenu indéfini), par exemple un tampon de profondeur.
        static std::expected<Image, std::string> CreateAttachment(const Device& device, VkFormat format, VkExtent2D extent,
                                                                  VkImageUsageFlags usage, VkImageAspectFlags aspect);

        // Texture cube vierge dont chaque face a sa propre vue 2D (GetFaceView), pour y dessiner.
        static std::expected<Image, std::string> CreateCubeAttachment(const Device& device, VkFormat format, std::uint32_t size,
                                                                      VkImageUsageFlags usage);

        // Crée l'image et y copie pixels (via un buffer intermédiaire) ; attend la fin de la copie.
        // mipmaps : réductions successives calculées par le GPU (filtre linéaire), pour une texture vue de loin
        // sans scintillement ; format filtrable en blit requis (RGBA8 UNORM ou SRGB : toujours le cas sur PC).
        // Cube : pixels contient les 6 faces à la suite (+X, -X, +Y, -Y, +Z, -Z), vue de type cube. Array : les
        // `layerCount` images à la suite.
        static std::expected<Image, std::string> CreateTexture(Device& device, VkFormat format, VkExtent2D extent,
                                                               std::span<const std::uint8_t> pixels, bool mipmaps = false,
                                                               ImageLayers kind = ImageLayers::Single,
                                                               std::uint32_t layerCount = 1);
        // De même avec ses `levels` niveaux de mip déjà calculés, à la suite dans pixels (chaque niveau : toutes les
        // couches) : formats compressés, que le GPU ne sait pas réduire. Taille de chaque niveau déduite du format.
        static std::expected<Image, std::string> CreateTextureLevels(Device& device, VkFormat format, VkExtent2D extent,
                                                                     std::span<const std::uint8_t> pixels, std::uint32_t levels,
                                                                     ImageLayers kind = ImageLayers::Single,
                                                                     std::uint32_t layerCount = 1);

        Image() = default;
        ~Image();

        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;
        Image(Image&& other) noexcept;
        Image& operator=(Image&& other) noexcept;

        VkImage GetHandle() const { return m_image; }
        VkImageView GetView() const { return m_view; }
        // Face i d'une image de CreateCubeAttachment (VK_NULL_HANDLE sinon).
        VkImageView GetFaceView(std::uint32_t face) const { return face < m_faceViews.size() ? m_faceViews[face] : VK_NULL_HANDLE; }
        VkExtent2D GetExtent() const { return m_extent; }
        std::uint32_t GetMipLevels() const { return m_mipLevels; }

        private:
        static std::expected<Image, std::string> Allocate(const Device& device, VkFormat format, VkExtent2D extent,
                                                          VkImageUsageFlags usage, VkImageAspectFlags aspect,
                                                          std::uint32_t mipLevels = 1, ImageLayers kind = ImageLayers::Single,
                                                          std::uint32_t layerCount = 1);
        void Reset();

        VkDevice m_device = VK_NULL_HANDLE;
        VmaAllocator m_allocator = VK_NULL_HANDLE;
        VkImage m_image = VK_NULL_HANDLE;
        VmaAllocation m_allocation = VK_NULL_HANDLE;
        VkImageView m_view = VK_NULL_HANDLE;
        std::array<VkImageView, 6> m_faceViews{}; // CreateCubeAttachment seulement
        VkExtent2D m_extent{};
        std::uint32_t m_mipLevels = 1;
    };
}
