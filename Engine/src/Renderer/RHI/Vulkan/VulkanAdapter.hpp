#pragma once

#include "Renderer/RHI/Vulkan/Vulkan.hpp"

#include <VkBootstrap.h>

#include <cstdint>
#include <expected>
#include <string>

namespace Flux::Vk {
    class Instance;

    // Choix du GPU (le dédié de préférence) et de ce qu'on lui demande : fonctions exigées (Vulkan 1.3, tables de
    // textures, dessin indirect, sémaphores à compteur) et facultatives, activées quand il les a.
    class Adapter {
        public:
        // surface VK_NULL_HANDLE : sans fenêtre (calculs, tests), le GPU n'a pas à savoir présenter.
        static std::expected<Adapter, std::string> Select(const Instance& instance, VkSurfaceKHR surface);

        const vkb::PhysicalDevice& Get() const { return m_physicalDevice; }
        VkPhysicalDevice GetHandle() const { return m_physicalDevice.physical_device; }
        const std::string& GetName() const { return m_physicalDevice.name; }
        // « 1.3 (GPU : 1.4.303) » : la version que le moteur utilise, puis la plus haute que le pilote accepte.
        const std::string& GetApiVersion() const { return m_apiVersion; }
        // Nanosecondes par tic des heures du GPU sur cette famille de files ; 0 si elle ne note pas l'heure.
        float GetTimestampPeriod(std::uint32_t family) const;

        // samplerAnisotropy activé : SamplerDesc::anisotropy permis.
        bool HasAnisotropy() const { return m_anisotropy; }
        // textureCompressionBC activé : formats BC7 (sol des planètes).
        bool HasTextureCompressionBC() const { return m_textureCompressionBC; }
        // VK_EXT_mesh_shader activée (étapes de tâche et de maillage) : sol des planètes en mesh shaders.
        bool HasMeshShaders() const { return m_meshShaders; }
        // maxPreferredMeshWorkGroupInvocations (0 sans mesh shaders).
        std::uint32_t GetMeshPreferredThreads() const { return m_meshPreferredThreads; }
        // Ray tracing par ray queries : VK_KHR_acceleration_structure, VK_KHR_ray_query (et VK_KHR_deferred_host_operations
        // qu'elle exige), avec les adresses des buffers (bufferDeviceAddress).
        bool HasRayTracing() const { return m_rayTracing; }
        // minAccelerationStructureScratchOffsetAlignment : alignement de la mémoire de travail d'une construction.
        std::uint32_t GetScratchAlignment() const { return m_scratchAlignment; }
        // VK_AMD_buffer_marker activé : le GPU peut noter où il en est (RhiCommandList::BeginStep).
        bool HasBufferMarkers() const { return m_bufferMarkers; }
        // VK_EXT_device_fault activée : le pilote décrit la panne après la perte du device.
        bool HasDeviceFault() const { return m_deviceFault; }

        private:
        vkb::PhysicalDevice m_physicalDevice;
        std::string m_apiVersion;
        bool m_anisotropy = false;
        bool m_textureCompressionBC = false;
        bool m_meshShaders = false;
        std::uint32_t m_meshPreferredThreads = 0;
        bool m_rayTracing = false;
        std::uint32_t m_scratchAlignment = 256;
        bool m_bufferMarkers = false;
        bool m_deviceFault = false;
    };
}
