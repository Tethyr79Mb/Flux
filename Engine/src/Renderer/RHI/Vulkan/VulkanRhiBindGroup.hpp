#pragma once

// RHI::BindGroupLayout et RHI::BindGroup : descriptor set layout, son pool, et ses sets.

#include "Flux/Renderer/RHI/BindGroup.hpp"
#include "Renderer/RHI/Vulkan/Vulkan.hpp"

#include <cstdint>

namespace Flux::Vk {
    // Le layout et son pool : ses groupes y sont pris, et y retournent un par un.
    class RhiBindGroupLayout final : public RHI::BindGroupLayout {
        public:
        RhiBindGroupLayout(VkDevice device, VkDescriptorSetLayout layout, VkDescriptorPool pool)
            : m_device(device), m_layout(layout), m_pool(pool) {}
        ~RhiBindGroupLayout() override;
        RhiBindGroupLayout(const RhiBindGroupLayout&) = delete;
        RhiBindGroupLayout& operator=(const RhiBindGroupLayout&) = delete;
        VkDescriptorSetLayout GetHandle() const { return m_layout; }
        VkDescriptorPool GetPool() const { return m_pool; }

        private:
        VkDevice m_device;
        VkDescriptorSetLayout m_layout;
        VkDescriptorPool m_pool;
    };

    class RhiBindGroup final : public RHI::BindGroup {
        public:
        RhiBindGroup(VkDevice device, VkDescriptorPool pool, VkDescriptorSet set) : m_device(device), m_pool(pool), m_set(set) {}
        ~RhiBindGroup() override { vkFreeDescriptorSets(m_device, m_pool, 1, &m_set); }
        RhiBindGroup(const RhiBindGroup&) = delete;
        RhiBindGroup& operator=(const RhiBindGroup&) = delete;
        void SetTexture(std::uint32_t slot, const RHI::Texture& texture, const RHI::Sampler& sampler) override;
        void SetBuffer(std::uint32_t slot, const RHI::Buffer& buffer) override;
        void SetTableTexture(std::uint32_t slot, std::uint32_t element, const RHI::Texture& texture, const RHI::Sampler& sampler) override;
        void SetStorageTexture(std::uint32_t slot, const RHI::Texture& texture) override;
        void SetAccelerationStructure(std::uint32_t slot, const RHI::AccelerationStructure& structure) override;
        VkDescriptorSet GetHandle() const { return m_set; }

        private:
        VkDevice m_device;
        VkDescriptorPool m_pool;
        VkDescriptorSet m_set;
    };
}
