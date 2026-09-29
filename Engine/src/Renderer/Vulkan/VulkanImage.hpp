#pragma once

#include "Renderer/Vulkan/Vulkan.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace Flux::Vk {
    class Device;

    // Image 2D allouée par VMA, avec sa vue (tous les niveaux de mip) : texture ou attachment de rendu.
    class Image {
        public:
        // Image vierge (contenu indéfini), par exemple un tampon de profondeur.
        static std::expected<Image, std::string> CreateAttachment(const Device& device, VkFormat format, VkExtent2D extent,
                                                                  VkImageUsageFlags usage, VkImageAspectFlags aspect);

        // Crée l'image et y copie pixels (via un buffer intermédiaire) ; attend la fin de la copie.
        // mipmaps : réductions successives calculées par le GPU (filtre linéaire), pour une texture vue de loin
        // sans scintillement ; format filtrable en blit requis (RGBA8 UNORM ou SRGB : toujours le cas sur PC).
        static std::expected<Image, std::string> CreateTexture(const Device& device, VkFormat format, VkExtent2D extent,
                                                               std::span<const std::uint8_t> pixels, bool mipmaps = false);

        Image() = default;
        ~Image();

        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;
        Image(Image&& other) noexcept;
        Image& operator=(Image&& other) noexcept;

        VkImage GetHandle() const { return m_image; }
        VkImageView GetView() const { return m_view; }
        VkExtent2D GetExtent() const { return m_extent; }
        std::uint32_t GetMipLevels() const { return m_mipLevels; }

        private:
        static std::expected<Image, std::string> Allocate(const Device& device, VkFormat format, VkExtent2D extent,
                                                          VkImageUsageFlags usage, VkImageAspectFlags aspect,
                                                          std::uint32_t mipLevels = 1);
        void Reset();

        VkDevice m_device = VK_NULL_HANDLE;
        VmaAllocator m_allocator = VK_NULL_HANDLE;
        VkImage m_image = VK_NULL_HANDLE;
        VmaAllocation m_allocation = VK_NULL_HANDLE;
        VkImageView m_view = VK_NULL_HANDLE;
        VkExtent2D m_extent{};
        std::uint32_t m_mipLevels = 1;
    };
}
