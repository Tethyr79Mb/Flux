#include "Renderer/Vulkan/VulkanImage.hpp"

#include "Renderer/Vulkan/VulkanBuffer.hpp"
#include "Renderer/Vulkan/VulkanDevice.hpp"

#include <algorithm>
#include <bit>
#include <format>
#include <utility>
#include <vector>

namespace Flux::Vk {
    std::expected<Image, std::string> Image::Allocate(const Device& device, VkFormat format, VkExtent2D extent,
                                                      VkImageUsageFlags usage, VkImageAspectFlags aspect,
                                                      std::uint32_t mipLevels, ImageLayers kind, std::uint32_t layerCount) {
        const bool cube = kind == ImageLayers::Cube;
        const bool volume = kind == ImageLayers::Volume;
        const std::uint32_t layers = cube ? 6u : kind == ImageLayers::Array ? layerCount : 1u;
        const std::uint32_t depth = volume ? layerCount : 1u;
        Image image;
        image.m_device = device.GetHandle();
        image.m_allocator = device.GetAllocator();
        image.m_extent = extent;
        image.m_depth = depth;
        image.m_layers = layers;
        image.m_mipLevels = mipLevels;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {extent.width, extent.height, depth};
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = layers;
        imageInfo.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            allocationInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT; // conseillé pour les cibles de rendu
        }
        if (!Check(vmaCreateImage(image.m_allocator, &imageInfo, &allocationInfo, &image.m_image, &image.m_allocation, nullptr),
                   "vmaCreateImage")) {
            return std::unexpected(std::format("allocation d'une image {}x{}", extent.width, extent.height));
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image.m_image;
        viewInfo.viewType = cube     ? VK_IMAGE_VIEW_TYPE_CUBE
                          : volume ? VK_IMAGE_VIEW_TYPE_3D
                          : kind == ImageLayers::Array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                       : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {aspect, 0, mipLevels, 0, layers};
        if (!Check(vkCreateImageView(image.m_device, &viewInfo, nullptr, &image.m_view), "vkCreateImageView")) {
            return std::unexpected("création de la vue d'une image");
        }
        return image;
    }

    std::expected<Image, std::string> Image::CreateAttachment(const Device& device, VkFormat format, VkExtent2D extent,
                                                              VkImageUsageFlags usage, VkImageAspectFlags aspect) {
        return Allocate(device, format, extent, usage, aspect);
    }

    std::expected<Image, std::string> Image::CreateVolume(const Device& device, VkFormat format, VkExtent2D extent,
                                                          std::uint32_t depth, bool mipmaps) {
        const std::uint32_t levels =
            mipmaps ? static_cast<std::uint32_t>(std::bit_width(std::max({extent.width, extent.height, depth}))) : 1u;
        auto image = Allocate(device, format, extent,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                  | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, levels, ImageLayers::Volume, depth);
        if (!image || levels == 1) {
            return image;
        }
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image->m_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!Check(vkCreateImageView(image->m_device, &viewInfo, nullptr, &image->m_storageView), "vkCreateImageView")) {
            return std::unexpected("création de la vue d'écriture d'une image 3D");
        }
        return image;
    }

    std::expected<Image, std::string> Image::CreateCubeAttachment(const Device& device, VkFormat format, std::uint32_t size,
                                                                  VkImageUsageFlags usage) {
        auto image = Allocate(device, format, {size, size}, usage, VK_IMAGE_ASPECT_COLOR_BIT, 1, ImageLayers::Cube);
        if (!image) {
            return image;
        }
        for (std::uint32_t face = 0; face < 6; ++face) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = image->m_image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, face, 1};
            if (!Check(vkCreateImageView(image->m_device, &viewInfo, nullptr, &image->m_faceViews[face]), "vkCreateImageView")) {
                return std::unexpected("création de la vue d'une face de texture cube");
            }
        }
        return image;
    }

    std::expected<Image, std::string> Image::CreateTexture(Device& device, VkFormat format, VkExtent2D extent,
                                                           std::span<const std::uint8_t> pixels, bool mipmaps, ImageLayers kind,
                                                           std::uint32_t layerCount) {
        const std::uint32_t layers = kind == ImageLayers::Cube ? 6u : kind == ImageLayers::Array ? layerCount : 1u;
        // Jusqu'à 1 x 1 : floor(log2(plus grand côté)) + 1 niveaux.
        const std::uint32_t levels = mipmaps ? static_cast<std::uint32_t>(std::bit_width(std::max(extent.width, extent.height))) : 1u;
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (levels > 1) {
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // chaque niveau est lu pour produire le suivant
        }
        auto image = Allocate(device, format, extent, usage, VK_IMAGE_ASPECT_COLOR_BIT, levels, kind, layerCount);
        if (!image) {
            return image;
        }

        // Faces ou couches à la suite dans pixels ; les niveaux suivants réduits par le GPU.
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
        region.imageExtent = {extent.width, extent.height, 1};
        Transfer::ImageWrite write;
        write.image = image->m_image;
        write.extent = extent;
        write.levels = levels;
        write.layers = layers;
        write.generateMips = levels > 1;
        write.regions = {&region, 1};
        const bool copied = device.UploadImage(write, pixels);
        if (!copied) {
            return std::unexpected("envoi d'une texture au GPU");
        }
        return image;
    }

    namespace {
        // Octets d'un niveau d'une couche : blocs de 4 x 4 pour les formats compressés, pixels sinon.
        std::uint64_t LevelBytes(VkFormat format, std::uint32_t width, std::uint32_t height) {
            switch (format) {
                case VK_FORMAT_BC7_UNORM_BLOCK:
                case VK_FORMAT_BC7_SRGB_BLOCK:
                    return std::uint64_t{(width + 3) / 4} * ((height + 3) / 4) * 16;
                case VK_FORMAT_R8G8B8A8_UNORM:
                case VK_FORMAT_R8G8B8A8_SRGB:
                    return std::uint64_t{width} * height * 4;
                default:
                    return 0;
            }
        }
    }

    std::expected<Image, std::string> Image::CreateTextureLevels(Device& device, VkFormat format, VkExtent2D extent,
                                                                 std::span<const std::uint8_t> pixels, std::uint32_t levels,
                                                                 ImageLayers kind, std::uint32_t layerCount) {
        const std::uint32_t layers = kind == ImageLayers::Cube ? 6u : kind == ImageLayers::Array ? layerCount : 1u;
        std::vector<VkBufferImageCopy> regions(levels);
        std::uint64_t offset = 0;
        for (std::uint32_t level = 0; level < levels; ++level) {
            const std::uint32_t width = std::max(extent.width >> level, 1u);
            const std::uint32_t height = std::max(extent.height >> level, 1u);
            const std::uint64_t bytes = LevelBytes(format, width, height);
            if (bytes == 0) {
                return std::unexpected("format de texture non pris en charge pour des mips donnés");
            }
            regions[level].bufferOffset = offset;
            regions[level].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers};
            regions[level].imageExtent = {width, height, 1};
            offset += bytes * layers;
        }
        if (offset != pixels.size()) {
            return std::unexpected(std::format("texture : {} octets donnés, {} attendus pour {} niveaux", pixels.size(), offset, levels));
        }
        auto image = Allocate(device, format, extent, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, levels, kind, layerCount);
        if (!image) {
            return image;
        }
        Transfer::ImageWrite write;
        write.image = image->m_image;
        write.extent = extent;
        write.levels = levels;
        write.layers = layers;
        write.regions = regions;
        const bool copied = device.UploadImage(write, pixels);
        if (!copied) {
            return std::unexpected("envoi d'une texture au GPU");
        }
        return image;
    }

    Image::~Image() {
        Reset();
    }

    Image::Image(Image&& other) noexcept
        : m_device(std::exchange(other.m_device, VK_NULL_HANDLE)),
          m_allocator(std::exchange(other.m_allocator, VK_NULL_HANDLE)),
          m_image(std::exchange(other.m_image, VK_NULL_HANDLE)),
          m_allocation(std::exchange(other.m_allocation, VK_NULL_HANDLE)),
          m_view(std::exchange(other.m_view, VK_NULL_HANDLE)),
          m_faceViews(std::exchange(other.m_faceViews, {})),
          m_storageView(std::exchange(other.m_storageView, VK_NULL_HANDLE)),
          m_extent(std::exchange(other.m_extent, {})),
          m_depth(std::exchange(other.m_depth, 1)),
          m_layers(std::exchange(other.m_layers, 1)),
          m_mipLevels(std::exchange(other.m_mipLevels, 1)) {}

    Image& Image::operator=(Image&& other) noexcept {
        if (this != &other) {
            Reset();
            m_device = std::exchange(other.m_device, VK_NULL_HANDLE);
            m_allocator = std::exchange(other.m_allocator, VK_NULL_HANDLE);
            m_image = std::exchange(other.m_image, VK_NULL_HANDLE);
            m_allocation = std::exchange(other.m_allocation, VK_NULL_HANDLE);
            m_view = std::exchange(other.m_view, VK_NULL_HANDLE);
            m_faceViews = std::exchange(other.m_faceViews, {});
            m_storageView = std::exchange(other.m_storageView, VK_NULL_HANDLE);
            m_extent = std::exchange(other.m_extent, {});
            m_depth = std::exchange(other.m_depth, 1);
            m_layers = std::exchange(other.m_layers, 1);
            m_mipLevels = std::exchange(other.m_mipLevels, 1);
        }
        return *this;
    }

    void Image::Reset() {
        for (VkImageView& face : m_faceViews) {
            if (face != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, face, nullptr);
                face = VK_NULL_HANDLE;
            }
        }
        if (m_storageView != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, m_storageView, nullptr);
            m_storageView = VK_NULL_HANDLE;
        }
        if (m_view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, m_view, nullptr);
        }
        if (m_image != VK_NULL_HANDLE) {
            vmaDestroyImage(m_allocator, m_image, m_allocation);
        }
        m_view = VK_NULL_HANDLE;
        m_image = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
    }
}
