#include "RHI/Vulkan/VulkanSwapchain.hpp"

#include "RHI/Vulkan/VulkanDevice.hpp"

namespace Flux::Vk {
    Swapchain::Swapchain(const Device& device) : m_device(device) {}

    Swapchain::~Swapchain() {
        DestroyImageResources();
        if (IsValid()) {
            vkb::destroy_swapchain(m_swapchain);
        }
    }

    bool Swapchain::Recreate(VkExtent2D extent) {
        vkb::SwapchainBuilder builder(m_device.GetBootstrapDevice());
        auto swapchain = builder
            // Couleurs écrites en linéaire, converties en sRGB par le matériel.
            .set_desired_format({VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
            .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
            .set_desired_extent(extent.width, extent.height)
            .set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
            .set_old_swapchain(m_swapchain)
            .build();
        if (!swapchain) {
            std::fprintf(stderr, "Vulkan : création de la swapchain : %s\n", swapchain.error().message().c_str());
            return false;
        }

        DestroyImageResources();
        if (IsValid()) {
            vkb::destroy_swapchain(m_swapchain);
        }
        m_swapchain = swapchain.value();

        auto images = m_swapchain.get_images();
        auto views = m_swapchain.get_image_views();
        if (!images || !views) {
            return false;
        }
        m_images = std::move(images.value());
        m_imageViews = std::move(views.value());

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        m_renderFinished.resize(m_images.size(), VK_NULL_HANDLE);
        for (VkSemaphore& semaphore : m_renderFinished) {
            if (!Check(vkCreateSemaphore(m_device.GetHandle(), &semaphoreInfo, nullptr, &semaphore), "vkCreateSemaphore")) {
                return false;
            }
        }
        return true;
    }

    void Swapchain::DestroyImageResources() {
        for (VkSemaphore semaphore : m_renderFinished) {
            vkDestroySemaphore(m_device.GetHandle(), semaphore, nullptr);
        }
        m_renderFinished.clear();
        if (!m_imageViews.empty()) {
            m_swapchain.destroy_image_views(m_imageViews);
            m_imageViews.clear();
        }
        m_images.clear();
    }
}
