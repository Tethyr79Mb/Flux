#pragma once

#include "Renderer/Vulkan/Vulkan.hpp"
#include "Renderer/Vulkan/VulkanBuffer.hpp"
#include "Renderer/Vulkan/VulkanTransfer.hpp"

#include <VkBootstrap.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

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
        VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice.physical_device; }
        const vkb::Device& GetBootstrapDevice() const { return m_device; }
        VkSurfaceKHR GetSurface() const { return m_surface; }
        const std::string& GetName() const { return m_physicalDevice.name; }
        // « 1.3 (GPU : 1.4.303) » : la version que le moteur utilise, puis la plus haute que le pilote accepte.
        const std::string& GetApiVersion() const { return m_apiVersion; }
        VmaAllocator GetAllocator() const { return m_allocator; }

        VkQueue GetGraphicsQueue() const { return m_graphicsQueue; }
        uint32_t GetGraphicsQueueFamily() const { return m_graphicsQueueFamily; }
        VkQueue GetPresentQueue() const { return m_presentQueue; }
        // Nanosecondes par tic des heures du GPU ; 0 si la file graphique ne note pas l'heure.
        float GetTimestampPeriod() const;
        // samplerAnisotropy activé : SamplerDesc::anisotropy permis.
        bool HasAnisotropy() const { return m_anisotropy; }
        // VK_EXT_mesh_shader activée (étapes de tâche et de maillage) : sol des planètes en mesh shaders.
        bool HasMeshShaders() const { return m_meshShaders; }
        // maxPreferredMeshWorkGroupInvocations (0 sans mesh shaders).
        std::uint32_t GetMeshPreferredThreads() const { return m_meshPreferredThreads; }
        // textureCompressionBC activé : formats BC7 (sol des planètes).
        bool HasTextureCompressionBC() const { return m_textureCompressionBC; }
        // VK_AMD_buffer_marker activé : le GPU peut noter où il en est (RhiCommandList::BeginStep).
        bool HasBufferMarkers() const { return m_bufferMarkers; }
        // VK_EXT_debug_utils activée : repères nommés dans les listes de commandes (sinon ses fonctions sont interdites).
        bool HasDebugLabels() const { return m_debugLabels; }
        // Après la perte du device : la description de la panne par le pilote (VK_EXT_device_fault), vide sans elle.
        std::string DescribeFault() const;

        void WaitIdle() const;

        // Enregistre des commandes, les soumet sur la file graphique et attend leur fin (après les envois en attente,
        // soumis avec elles). Bloque le CPU : calculs dont on attend le résultat, tests.
        bool ImmediateSubmit(const std::function<void(VkCommandBuffer)>& record);

        // Envois vers la mémoire vidéo (VulkanTransfer) : copiés par la file de transfert, attendus par la prochaine
        // soumission à la file graphique (image ou ImmediateSubmit), sans attendre le GPU. Sans envois différés
        // (SetDeferredUploads, pas de fenêtre), soumis et attendus tout de suite.
        // inUse : le buffer sert peut-être à une image déjà soumise, la copie attend qu'elle soit finie.
        bool UploadBuffer(VkBuffer target, VkDeviceSize offset, const void* data, VkDeviceSize size, bool inUse);
        bool UploadImage(const Transfer::ImageWrite& write, std::span<const std::uint8_t> pixels);
        void SetDeferredUploads(bool deferred) { m_deferUploads = deferred; }
        Transfer& GetTransfer() { return *m_transfer; }
        // Familles de files qui se partagent les buffers remplis par envoi (Buffer::Create) ; vide avec une seule file.
        std::span<const std::uint32_t> GetSharedFamilies() const { return m_transfer->GetSharedFamilies(); }

        private:
        Device() = default;

        vkb::Instance m_instance;
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        vkb::PhysicalDevice m_physicalDevice;
        std::string m_apiVersion;
        vkb::Device m_device;
        VmaAllocator m_allocator = VK_NULL_HANDLE;

        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        uint32_t m_graphicsQueueFamily = 0;
        VkQueue m_presentQueue = VK_NULL_HANDLE;

        VkCommandPool m_immediatePool = VK_NULL_HANDLE;
        std::unique_ptr<Transfer> m_transfer;
        bool m_deferUploads = false;
        bool m_anisotropy = false;
        bool m_meshShaders = false;
        std::uint32_t m_meshPreferredThreads = 0;
        bool m_textureCompressionBC = false;
        bool m_bufferMarkers = false;
        bool m_deviceFault = false;
        bool m_debugLabels = false;
    };
}
