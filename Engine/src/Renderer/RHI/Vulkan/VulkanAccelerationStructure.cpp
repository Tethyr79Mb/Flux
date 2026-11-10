#include "Renderer/RHI/Vulkan/VulkanAccelerationStructure.hpp"

#include "Renderer/RHI/Vulkan/VulkanDevice.hpp"
#include "Renderer/RHI/Vulkan/VulkanRhiBuffer.hpp"

#include <algorithm>
#include <utility>

namespace Flux::Vk {
    VkDeviceAddress GetBufferAddress(VkDevice device, VkBuffer buffer) {
        VkBufferDeviceAddressInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = buffer;
        return vkGetBufferDeviceAddress(device, &info);
    }

    std::expected<std::unique_ptr<RhiAccelerationStructure>, std::string>
    RhiAccelerationStructure::CreateBottom(const Device& device, std::span<const RHI::AccelerationGeometry> geometries) {
        if (geometries.empty()) {
            return std::unexpected("structure d'accélération sans triangles");
        }
        std::unique_ptr<RhiAccelerationStructure> structure(new RhiAccelerationStructure());
        structure->m_device = device.GetHandle();
        structure->m_geometries.reserve(geometries.size());
        structure->m_primitiveCounts.reserve(geometries.size());
        for (const RHI::AccelerationGeometry& source : geometries) {
            const VkBuffer vertices = static_cast<const RhiBuffer&>(*source.vertices).GetHandle();
            const VkBuffer indices = static_cast<const RhiBuffer&>(*source.indices).GetHandle();
            VkAccelerationStructureGeometryKHR geometry{};
            geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            // Opaque : le GPU s'arrête au premier triangle touché sans demander au shader. Sinon, chaque triangle
            // rencontré une seule fois (le seuil d'alpha ne compte pas deux fois le même).
            geometry.flags = source.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
            VkAccelerationStructureGeometryTrianglesDataKHR& triangles = geometry.geometry.triangles;
            triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            triangles.vertexData.deviceAddress = GetBufferAddress(structure->m_device, vertices) + source.vertexOffset;
            triangles.vertexStride = source.vertexStride;
            triangles.maxVertex = std::max(source.vertexCount, 1u) - 1;
            triangles.indexType = VK_INDEX_TYPE_UINT32;
            triangles.indexData.deviceAddress = GetBufferAddress(structure->m_device, indices) + source.indexOffset;
            structure->m_geometries.push_back(geometry);
            structure->m_primitiveCounts.push_back(source.indexCount / 3);
        }
        if (auto allocated = structure->Allocate(device, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR); !allocated) {
            return std::unexpected(allocated.error());
        }
        return structure;
    }

    std::expected<std::unique_ptr<RhiAccelerationStructure>, std::string>
    RhiAccelerationStructure::CreateTop(const Device& device, std::uint32_t maxInstances) {
        std::unique_ptr<RhiAccelerationStructure> structure(new RhiAccelerationStructure());
        structure->m_device = device.GetHandle();
        structure->m_top = true;
        structure->m_maxInstances = std::max(maxInstances, 1u);
        auto instances = Buffer::Create(device.GetAllocator(),
                                        VkDeviceSize{structure->m_maxInstances} * sizeof(VkAccelerationStructureInstanceKHR),
                                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                                            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                        Buffer::Memory::Upload, {}, 16); // adresse des instances : multiple de 16 (VUID 03715)
        if (!instances) {
            return std::unexpected(instances.error());
        }
        structure->m_instances = std::move(*instances);
        VkAccelerationStructureGeometryKHR geometry{};
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geometry.geometry.instances.arrayOfPointers = VK_FALSE;
        geometry.geometry.instances.data.deviceAddress = GetBufferAddress(structure->m_device, structure->m_instances.GetHandle());
        structure->m_geometries.push_back(geometry);
        structure->m_primitiveCounts.push_back(structure->m_maxInstances);
        if (auto allocated = structure->Allocate(device, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR); !allocated) {
            return std::unexpected(allocated.error());
        }
        return structure;
    }

    RhiAccelerationStructure::~RhiAccelerationStructure() {
        if (m_handle != VK_NULL_HANDLE) {
            vkDestroyAccelerationStructureKHR(m_device, m_handle, nullptr);
        }
    }

    VkAccelerationStructureBuildGeometryInfoKHR RhiAccelerationStructure::DescribeBuild(VkAccelerationStructureTypeKHR type) const {
        VkAccelerationStructureBuildGeometryInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        info.type = type;
        // Rayons rapides plutôt que construction rapide : une structure basse se construit une fois ; la haute, petite,
        // reste bon marché à reconstruire à chaque image.
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = static_cast<std::uint32_t>(m_geometries.size());
        info.pGeometries = m_geometries.data();
        info.dstAccelerationStructure = m_handle;
        return info;
    }

    std::expected<void, std::string> RhiAccelerationStructure::Allocate(const Device& device, VkAccelerationStructureTypeKHR type) {
        const VkAccelerationStructureBuildGeometryInfoKHR info = DescribeBuild(type);
        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
                                                m_primitiveCounts.data(), &sizes);
        auto storage = Buffer::Create(device.GetAllocator(), std::max<VkDeviceSize>(sizes.accelerationStructureSize, 256),
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      Buffer::Memory::GpuOnly);
        if (!storage) {
            return std::unexpected(storage.error());
        }
        m_storage = std::move(*storage);
        m_scratchSize = sizes.buildScratchSize;
        m_scratchAlignment = device.GetAdapter().GetScratchAlignment();

        VkAccelerationStructureCreateInfoKHR create{};
        create.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        create.buffer = m_storage.GetHandle();
        create.size = sizes.accelerationStructureSize;
        create.type = type;
        if (!Check(vkCreateAccelerationStructureKHR(m_device, &create, nullptr, &m_handle), "vkCreateAccelerationStructureKHR")) {
            return std::unexpected("création d'une structure d'accélération");
        }
        VkAccelerationStructureDeviceAddressInfoKHR address{};
        address.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        address.accelerationStructure = m_handle;
        m_address = vkGetAccelerationStructureDeviceAddressKHR(m_device, &address);
        return {};
    }

    void RhiAccelerationStructure::RecordBuild(VkCommandBuffer commandBuffer, VkBuffer scratchBuffer, VkDeviceSize offset,
                                               std::span<const RHI::AccelerationInstance> instances) {
        const VkDeviceAddress scratch = GetBufferAddress(m_device, scratchBuffer) + offset;
        VkAccelerationStructureBuildGeometryInfoKHR info =
            DescribeBuild(m_top ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
        info.scratchData.deviceAddress = (scratch + m_scratchAlignment - 1) / m_scratchAlignment * m_scratchAlignment;

        if (m_top) {
            // Écrites une à une dans la mémoire de la structure : le GPU ne la lit plus (CommandList::BuildTopLevel).
            const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(instances.size(), m_maxInstances));
            auto* target = static_cast<VkAccelerationStructureInstanceKHR*>(m_instances.GetMappedData());
            for (std::uint32_t i = 0; i < count; ++i) {
                const RHI::AccelerationInstance& source = instances[i];
                VkAccelerationStructureInstanceKHR instance{};
                for (int row = 0; row < 3; ++row) {
                    for (int column = 0; column < 4; ++column) {
                        instance.transform.matrix[row][column] = source.transform[row][column];
                    }
                }
                instance.instanceCustomIndex = source.customIndex & 0xFFFFFFu;
                instance.mask = source.mask;
                instance.instanceShaderBindingTableRecordOffset = 0;
                // Les deux faces : une ombre ne dépend pas du sens dans lequel le rayon traverse la surface.
                instance.flags = static_cast<VkGeometryInstanceFlagsKHR>(VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR);
                instance.accelerationStructureReference = static_cast<const RhiAccelerationStructure*>(source.bottom)->GetAddress();
                target[i] = instance;
            }
            m_instances.Flush(0, VkDeviceSize{count} * sizeof(VkAccelerationStructureInstanceKHR));
            const VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
            const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
            vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &info, &ranges);
            return;
        }

        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(m_primitiveCounts.size());
        for (std::size_t g = 0; g < ranges.size(); ++g) {
            ranges[g] = {m_primitiveCounts[g], 0, 0, 0};
        }
        const VkAccelerationStructureBuildRangeInfoKHR* rangePointer = ranges.data();
        vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &info, &rangePointer);
        // Construite une fois : la description (adresses des sommets) ne sert plus.
        m_geometries = {};
        m_primitiveCounts = {};
    }
}
