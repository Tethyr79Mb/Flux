#pragma once

// RHI::AccelerationStructure sur VK_KHR_acceleration_structure.

#include "Flux/Renderer/RHI/AccelerationStructure.hpp"
#include "Renderer/RHI/Vulkan/Vulkan.hpp"
#include "Renderer/RHI/Vulkan/VulkanBuffer.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Flux::Vk {
    class Device;

    // Adresse d'un buffer créé avec VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
    VkDeviceAddress GetBufferAddress(VkDevice device, VkBuffer buffer);

    class RhiAccelerationStructure final : public RHI::AccelerationStructure {
        public:
        static std::expected<std::unique_ptr<RhiAccelerationStructure>, std::string>
        CreateBottom(const Device& device, std::span<const RHI::AccelerationGeometry> geometries);
        static std::expected<std::unique_ptr<RhiAccelerationStructure>, std::string> CreateTop(const Device& device,
                                                                                             std::uint32_t maxInstances);
        ~RhiAccelerationStructure() override;

        RhiAccelerationStructure(const RhiAccelerationStructure&) = delete;
        RhiAccelerationStructure& operator=(const RhiAccelerationStructure&) = delete;

        std::uint64_t GetSize() const override { return m_storage.GetSize(); }
        std::uint64_t GetScratchSize() const override { return m_scratchSize + m_scratchAlignment; }
        std::uint32_t GetMaxInstances() const override { return m_maxInstances; }
        VkAccelerationStructureKHR GetHandle() const { return m_handle; }
        VkDeviceAddress GetAddress() const { return m_address; }

        // Enregistre la construction, sa place de travail à `offset` dans `scratch` (alignée ici : GetScratchSize octets
        // marge comprise). Structure haute : `instances` écrites d'abord dans sa mémoire.
        void RecordBuild(VkCommandBuffer commandBuffer, VkBuffer scratch, VkDeviceSize offset,
                         std::span<const RHI::AccelerationInstance> instances = {});

        private:
        RhiAccelerationStructure() = default;
        // Crée le buffer et la structure à la taille demandée par la description (m_geometries, m_primitiveCounts).
        std::expected<void, std::string> Allocate(const Device& device, VkAccelerationStructureTypeKHR type);
        VkAccelerationStructureBuildGeometryInfoKHR DescribeBuild(VkAccelerationStructureTypeKHR type) const;

        VkDevice m_device = VK_NULL_HANDLE;
        Buffer m_storage;
        VkAccelerationStructureKHR m_handle = VK_NULL_HANDLE;
        VkDeviceAddress m_address = 0;
        VkDeviceSize m_scratchSize = 0;
        VkDeviceSize m_scratchAlignment = 1;
        bool m_top = false;
        // Structure basse : ses géométries, gardées pour la construction ; haute : la seule géométrie d'instances.
        std::vector<VkAccelerationStructureGeometryKHR> m_geometries;
        std::vector<std::uint32_t> m_primitiveCounts;
        // Structure haute : instances écrites par le CPU (VkAccelerationStructureInstanceKHR).
        Buffer m_instances;
        std::uint32_t m_maxInstances = 0;
    };
}
