#pragma once

#include "RHI/Vulkan/Vulkan.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace Flux::Vk {
    class Device;

    // Image 2D (une seule mip) allouée par VMA, avec sa vue : texture ou attachment de rendu.
    class Image {
        public:
        // Image vierge (contenu indéfini), par exemple un tampon de profondeur.
        static std::expected<Image, std::string> CreateAttachment(const Device& device, VkFormat format, VkExtent2D extent,
                                                                  VkImageUsageFlags usage, VkImageAspectFlags aspect);

        // Crée l'image et y copie pixels (via un buffer intermédiaire) ; attend la fin de la copie.
        static std::expected<Image, std::string> CreateTexture(const Device& device, VkFormat format, VkExtent2D extent,
                                                               std::span<const std::uint8_t> pixels);

        Image() = default;
        ~Image();

        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;
        Image(Image&& other) noexcept;
        Image& operator=(Image&& other) noexcept;

        VkImage GetHandle() const { return m_image; }
        VkImageView GetView() const { return m_view; }
        VkExtent2D GetExtent() const { return m_extent; }

        private:
        static std::expected<Image, std::string> Allocate(const Device& device, VkFormat format, VkExtent2D extent,
                                                          VkImageUsageFlags usage, VkImageAspectFlags aspect);
        void Reset();

        VkDevice m_device = VK_NULL_HANDLE;
        VmaAllocator m_allocator = VK_NULL_HANDLE;
        VkImage m_image = VK_NULL_HANDLE;
        VmaAllocation m_allocation = VK_NULL_HANDLE;
        VkImageView m_view = VK_NULL_HANDLE;
        VkExtent2D m_extent{};
    };
}
