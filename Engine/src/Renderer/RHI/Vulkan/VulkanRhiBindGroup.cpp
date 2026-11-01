#include "Renderer/RHI/Vulkan/VulkanRhiBindGroup.hpp"

#include "Renderer/RHI/Vulkan/VulkanRhiBuffer.hpp"
#include "Renderer/RHI/Vulkan/VulkanRhiTexture.hpp"

namespace Flux::Vk {
    RhiBindGroupLayout::~RhiBindGroupLayout() {
        vkDestroyDescriptorPool(m_device, m_pool, nullptr); // ses groupes doivent déjà être rendus
        vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    }

    void RhiBindGroup::SetTexture(std::uint32_t slot, const RHI::Texture& texture, const RHI::Sampler& sampler) {
        const VkDescriptorImageInfo imageInfo{static_cast<const RhiSampler&>(sampler).GetHandle(),
                                              static_cast<const RhiTexture&>(texture).GetImage().GetView(),
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_set;
        write.dstBinding = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }

    void RhiBindGroup::SetBuffer(std::uint32_t slot, const RHI::Buffer& buffer) {
        const VkDescriptorBufferInfo bufferInfo{static_cast<const RhiBuffer&>(buffer).GetHandle(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_set;
        write.dstBinding = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }

    void RhiBindGroup::SetTableTexture(std::uint32_t slot, std::uint32_t element, const RHI::Texture& texture,
                                       const RHI::Sampler& sampler) {
        const VkDescriptorImageInfo imageInfo{static_cast<const RhiSampler&>(sampler).GetHandle(),
                                              static_cast<const RhiTexture&>(texture).GetImage().GetView(),
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_set;
        write.dstBinding = slot;
        write.dstArrayElement = element;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }

    void RhiBindGroup::SetStorageTexture(std::uint32_t slot, const RHI::Texture& texture) {
        const VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, static_cast<const RhiTexture&>(texture).GetImage().GetStorageView(),
                                              VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_set;
        write.dstBinding = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }
}
