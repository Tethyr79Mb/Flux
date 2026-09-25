#include "RHI/Vulkan/VulkanImage.hpp"

#include "RHI/Vulkan/VulkanBuffer.hpp"
#include "RHI/Vulkan/VulkanDevice.hpp"

#include <cstring>
#include <utility>

namespace Flux::Vk {
    std::expected<Image, std::string> Image::CreateTexture(const Device& device, VkFormat format, VkExtent2D extent,
                                                           std::span<const std::uint8_t> pixels) {
        auto staging = Buffer::Create(device.GetAllocator(), pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Memory::Upload);
        if (!staging) {
            return std::unexpected(staging.error());
        }
        std::memcpy(staging->GetMappedData(), pixels.data(), pixels.size());

        Image image;
        image.m_device = device.GetHandle();
        image.m_allocator = device.GetAllocator();
        image.m_extent = extent;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {extent.width, extent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (!Check(vmaCreateImage(image.m_allocator, &imageInfo, &allocationInfo, &image.m_image, &image.m_allocation, nullptr),
                   "vmaCreateImage")) {
            return std::unexpected("allocation d'une texture");
        }

        const bool copied = device.ImmediateSubmit([&](VkCommandBuffer commandBuffer) {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.m_image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(commandBuffer, &dependency);

            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyBufferToImage(commandBuffer, staging->GetHandle(), image.m_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
        });
        if (!copied) {
            return std::unexpected("envoi d'une texture au GPU");
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image.m_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!Check(vkCreateImageView(image.m_device, &viewInfo, nullptr, &image.m_view), "vkCreateImageView")) {
            return std::unexpected("création de la vue d'une texture");
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
          m_extent(std::exchange(other.m_extent, {})) {}

    Image& Image::operator=(Image&& other) noexcept {
        if (this != &other) {
            Reset();
            m_device = std::exchange(other.m_device, VK_NULL_HANDLE);
            m_allocator = std::exchange(other.m_allocator, VK_NULL_HANDLE);
            m_image = std::exchange(other.m_image, VK_NULL_HANDLE);
            m_allocation = std::exchange(other.m_allocation, VK_NULL_HANDLE);
            m_view = std::exchange(other.m_view, VK_NULL_HANDLE);
            m_extent = std::exchange(other.m_extent, {});
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
