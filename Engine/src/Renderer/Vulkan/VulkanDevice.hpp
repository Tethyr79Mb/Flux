#pragma once

#include "Renderer/Vulkan/Vulkan.hpp"

#include <VkBootstrap.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace Flux::Vk {
    // Instance, surface, device, files et allocateur : tout ce qui vit aussi longtemps que le renderer.
    class Device {
        public:
        // nativeWindowHandle nul : sans surface ni file de présentation (calculs, tests).
        static std::expected<std::unique_ptr<Device>, std::string> Create(void* nativeWindowHandle);

        ~Device();

        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;

        VkDevice GetHandle() const { return m_device.device; }
        const vkb::Device& GetBootstrapDevice() const { return m_device; }
        VkSurfaceKHR GetSurface() const { return m_surface; }
        const std::string& GetName() const { return m_physicalDevice.name; }
        VmaAllocator GetAllocator() const { return m_allocator; }

        VkQueue GetGraphicsQueue() const { return m_graphicsQueue; }
        uint32_t GetGraphicsQueueFamily() const { return m_graphicsQueueFamily; }
        VkQueue GetPresentQueue() const { return m_presentQueue; }
        // Nanosecondes par tic des heures du GPU ; 0 si la file graphique ne note pas l'heure.
        float GetTimestampPeriod() const;

        void WaitIdle() const;

        // Enregistre des commandes, les soumet sur la file graphique et attend leur fin.
        // Réservé au chargement (textures, buffers statiques) : bloque le CPU.
        bool ImmediateSubmit(const std::function<void(VkCommandBuffer)>& record) const;

        private:
        Device() = default;

        vkb::Instance m_instance;
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        vkb::PhysicalDevice m_physicalDevice;
        vkb::Device m_device;
        VmaAllocator m_allocator = VK_NULL_HANDLE;

        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        uint32_t m_graphicsQueueFamily = 0;
        VkQueue m_presentQueue = VK_NULL_HANDLE;

        VkCommandPool m_immediatePool = VK_NULL_HANDLE;
    };
}
