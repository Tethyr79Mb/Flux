#pragma once

#include "Renderer/RHI/Vulkan/Vulkan.hpp"
#include "Renderer/RHI/Vulkan/VulkanAdapter.hpp"
#include "Renderer/RHI/Vulkan/VulkanBuffer.hpp"
#include "Renderer/RHI/Vulkan/VulkanInstance.hpp"
#include "Renderer/RHI/Vulkan/VulkanSurface.hpp"
#include "Renderer/RHI/Vulkan/VulkanTransfer.hpp"

#include <VkBootstrap.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Flux::Vk {
    // Device logique, ses files (graphique, présentation, transfert) et l'allocateur, sur l'instance, la surface et le
    // GPU choisi (Adapter) qu'il possède : tout ce qui vit aussi longtemps que le renderer.
    class Device {
        public:
        // nativeWindowHandle nul : sans surface ni file de présentation (calculs, tests).
        static std::expected<std::unique_ptr<Device>, std::string> Create(void* nativeWindowHandle);

        ~Device();

        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;

        VkDevice GetHandle() const { return m_device.device; }
        VkPhysicalDevice GetPhysicalDevice() const { return m_adapter.GetHandle(); }
        const vkb::Device& GetBootstrapDevice() const { return m_device; }
        // VK_NULL_HANDLE sans fenêtre.
        VkSurfaceKHR GetSurface() const { return m_surface ? m_surface->GetHandle() : VK_NULL_HANDLE; }
        const Adapter& GetAdapter() const { return m_adapter; }
        const std::string& GetName() const { return m_adapter.GetName(); }
        const std::string& GetApiVersion() const { return m_adapter.GetApiVersion(); }
        VmaAllocator GetAllocator() const { return m_allocator; }

        VkQueue GetGraphicsQueue() const { return m_graphicsQueue; }
        uint32_t GetGraphicsQueueFamily() const { return m_graphicsQueueFamily; }
        VkQueue GetPresentQueue() const { return m_presentQueue; }
        // Nanosecondes par tic des heures du GPU ; 0 si la file graphique ne note pas l'heure.
        float GetTimestampPeriod() const;
        // Fonctions facultatives du GPU (Adapter) et de l'instance.
        bool HasAnisotropy() const { return m_adapter.HasAnisotropy(); }
        bool HasMeshShaders() const { return m_adapter.HasMeshShaders(); }
        std::uint32_t GetMeshPreferredThreads() const { return m_adapter.GetMeshPreferredThreads(); }
        bool HasTextureCompressionBC() const { return m_adapter.HasTextureCompressionBC(); }
        bool HasRayTracing() const { return m_adapter.HasRayTracing(); }
        bool HasBufferMarkers() const { return m_adapter.HasBufferMarkers(); }
        bool HasDebugLabels() const { return m_instance->HasDebugLabels(); }
        // Après la perte du device : la description de la panne par le pilote (VK_EXT_device_fault), vide sans elle.
        std::string DescribeFault() const;

        void WaitIdle() const;

        // Enregistre des commandes, les soumet sur la file graphique et attend leur fin (après les envois en attente,
        // soumis avec elles). Bloque le CPU : calculs dont on attend le résultat, tests.
        bool ImmediateSubmit(const std::function<void(VkCommandBuffer)>& record);
        // De même pour des command buffers déjà enregistrés et fermés, joués dans l'ordre. recorded faux : leur
        // enregistrement a échoué, rien n'est soumis (les envois en attente sont rendus).
        bool SubmitAndWait(std::span<const VkCommandBuffer> commandBuffers, bool recorded = true);

        // Envois vers la mémoire vidéo (VulkanTransfer) : copiés par la file de transfert, attendus par la prochaine
        // soumission à la file graphique (image ou ImmediateSubmit), sans attendre le GPU. Sans envois différés
        // (SetDeferredUploads, pas de fenêtre), soumis et attendus tout de suite.
        // inUse : le buffer sert peut-être à une image déjà soumise, la copie attend qu'elle soit finie.
        bool UploadBuffer(VkBuffer target, VkDeviceSize offset, const void* data, VkDeviceSize size, bool inUse);
        bool UploadImage(const Transfer::ImageWrite& write, std::span<const std::uint8_t> pixels);
        // Depuis une mémoire d'envoi déjà remplie (Transfer::WriteImage), sans recopie.
        bool UploadImage(const Transfer::ImageWrite& write, VkBuffer source, VkDeviceSize sourceOffset);
        void SetDeferredUploads(bool deferred) { m_deferUploads = deferred; }
        Transfer& GetTransfer() { return *m_transfer; }
        // Familles de files qui se partagent les buffers remplis par envoi (Buffer::Create) ; vide avec une seule file.
        std::span<const std::uint32_t> GetSharedFamilies() const { return m_transfer->GetSharedFamilies(); }

        private:
        Device() = default;

        // Dans cet ordre : détruits après le device (instance en dernier).
        std::unique_ptr<Instance> m_instance;
        std::unique_ptr<Surface> m_surface; // nul sans fenêtre
        Adapter m_adapter;
        vkb::Device m_device;
        VmaAllocator m_allocator = VK_NULL_HANDLE;

        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        uint32_t m_graphicsQueueFamily = 0;
        VkQueue m_presentQueue = VK_NULL_HANDLE;

        VkCommandPool m_immediatePool = VK_NULL_HANDLE;
        std::unique_ptr<Transfer> m_transfer;
        bool m_deferUploads = false;
    };
}
