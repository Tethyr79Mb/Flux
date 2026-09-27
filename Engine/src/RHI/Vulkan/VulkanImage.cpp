#include "RHI/Vulkan/VulkanImage.hpp"

#include "RHI/Vulkan/VulkanBuffer.hpp"
#include "RHI/Vulkan/VulkanDevice.hpp"

#include <algorithm>
#include <bit>
#include <format>
#include <utility>

namespace Flux::Vk {
    std::expected<Image, std::string> Image::Allocate(const Device& device, VkFormat format, VkExtent2D extent,
                                                      VkImageUsageFlags usage, VkImageAspectFlags aspect,
                                                      std::uint32_t mipLevels) {
        Image image;
        image.m_device = device.GetHandle();
        image.m_allocator = device.GetAllocator();
        image.m_extent = extent;
        image.m_mipLevels = mipLevels;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {extent.width, extent.height, 1};
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = 1;
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
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {aspect, 0, mipLevels, 0, 1};
        if (!Check(vkCreateImageView(image.m_device, &viewInfo, nullptr, &image.m_view), "vkCreateImageView")) {
            return std::unexpected("création de la vue d'une image");
        }
        return image;
    }

    std::expected<Image, std::string> Image::CreateAttachment(const Device& device, VkFormat format, VkExtent2D extent,
                                                              VkImageUsageFlags usage, VkImageAspectFlags aspect) {
        return Allocate(device, format, extent, usage, aspect);
    }

    std::expected<Image, std::string> Image::CreateTexture(const Device& device, VkFormat format, VkExtent2D extent,
                                                           std::span<const std::uint8_t> pixels, bool mipmaps) {
        auto staging = Buffer::Create(device.GetAllocator(), pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Memory::Upload);
        if (!staging) {
            return std::unexpected(staging.error());
        }
        staging->Write(pixels.data(), pixels.size());

        // Jusqu'à 1 x 1 : floor(log2(plus grand côté)) + 1 niveaux.
        const std::uint32_t levels = mipmaps ? static_cast<std::uint32_t>(std::bit_width(std::max(extent.width, extent.height))) : 1u;
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (levels > 1) {
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // chaque niveau est lu pour produire le suivant
        }
        auto image = Allocate(device, format, extent, usage, VK_IMAGE_ASPECT_COLOR_BIT, levels);
        if (!image) {
            return image;
        }

        const bool copied = device.ImmediateSubmit([&](VkCommandBuffer commandBuffer) {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image->m_image;
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            const auto transition = [&](std::uint32_t level, std::uint32_t count, VkImageLayout from, VkImageLayout to,
                                        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                        VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
                barrier.srcStageMask = srcStage;
                barrier.srcAccessMask = srcAccess;
                barrier.dstStageMask = dstStage;
                barrier.dstAccessMask = dstAccess;
                barrier.oldLayout = from;
                barrier.newLayout = to;
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, 1};
                vkCmdPipelineBarrier2(commandBuffer, &dependency);
            };

            // Tous les niveaux prêts à recevoir une copie ; le niveau 0 reçoit les pixels.
            transition(0, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyBufferToImage(commandBuffer, staging->GetHandle(), image->m_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            // Chaque niveau est une réduction de moitié du précédent, qui passe ensuite en lecture.
            std::int32_t width = static_cast<std::int32_t>(extent.width);
            std::int32_t height = static_cast<std::int32_t>(extent.height);
            for (std::uint32_t level = 1; level < levels; ++level) {
                transition(level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                const std::int32_t nextWidth = std::max(width / 2, 1);
                const std::int32_t nextHeight = std::max(height / 2, 1);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
                blit.srcOffsets[1] = {width, height, 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                blit.dstOffsets[1] = {nextWidth, nextHeight, 1};
                vkCmdBlitImage(commandBuffer, image->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               image->m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                transition(level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                width = nextWidth;
                height = nextHeight;
            }
            // Le dernier niveau n'a servi que de destination.
            transition(levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
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
          m_extent(std::exchange(other.m_extent, {})),
          m_mipLevels(std::exchange(other.m_mipLevels, 1)) {}

    Image& Image::operator=(Image&& other) noexcept {
        if (this != &other) {
            Reset();
            m_device = std::exchange(other.m_device, VK_NULL_HANDLE);
            m_allocator = std::exchange(other.m_allocator, VK_NULL_HANDLE);
            m_image = std::exchange(other.m_image, VK_NULL_HANDLE);
            m_allocation = std::exchange(other.m_allocation, VK_NULL_HANDLE);
            m_view = std::exchange(other.m_view, VK_NULL_HANDLE);
            m_extent = std::exchange(other.m_extent, {});
            m_mipLevels = std::exchange(other.m_mipLevels, 1);
        }
        return *this;
    }

    void Image::Reset() {
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
