#pragma once

#include "Renderer/Vulkan/Vulkan.hpp"

#include <VkBootstrap.h>

#include <vector>

namespace Flux::Vk {
    class Device;

    class Swapchain {
        public:
        explicit Swapchain(const Device& device);
        ~Swapchain();

        Swapchain(const Swapchain&) = delete;
        Swapchain& operator=(const Swapchain&) = delete;

        // (Re)crée la swapchain ; le GPU ne doit plus utiliser l'ancienne (WaitIdle avant).
        // vsync false : MAILBOX (sans déchirure) si le pilote l'offre, sinon IMMEDIATE, sinon FIFO.
        bool Recreate(VkExtent2D extent, bool vsync);

        bool IsValid() const { return m_swapchain.swapchain != VK_NULL_HANDLE; }
        VkSwapchainKHR GetHandle() const { return m_swapchain.swapchain; }
        VkExtent2D GetExtent() const { return m_swapchain.extent; }
        VkFormat GetFormat() const { return m_swapchain.image_format; }
        // Mode réellement obtenu, qui peut différer du mode demandé.
        VkPresentModeKHR GetPresentMode() const { return m_swapchain.present_mode; }

        uint32_t GetImageCount() const { return static_cast<uint32_t>(m_images.size()); }
        VkImage GetImage(uint32_t index) const { return m_images[index]; }
        VkImageView GetImageView(uint32_t index) const { return m_imageViews[index]; }

        // Un sémaphore par image : il ne peut être réutilisé qu'une fois cette image de nouveau acquise.
        VkSemaphore GetRenderFinishedSemaphore(uint32_t index) const { return m_renderFinished[index]; }

        private:
        void DestroyImageResources();

        const Device& m_device;
        vkb::Swapchain m_swapchain;
        std::vector<VkImage> m_images;
        std::vector<VkImageView> m_imageViews;
        std::vector<VkSemaphore> m_renderFinished;
    };
}
