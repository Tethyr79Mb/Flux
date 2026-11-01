#pragma once

// RHI::Buffer et RHI::StagingBuffer sur un Vk::Buffer.

#include "Flux/Renderer/RHI/Buffer.hpp"
#include "Renderer/RHI/Vulkan/VulkanBuffer.hpp"

#include <cstdint>
#include <utility>

namespace Flux::Vk {
    class RhiBuffer final : public RHI::Buffer {
        public:
        explicit RhiBuffer(Vk::Buffer buffer) : m_buffer(std::move(buffer)) {}
        std::uint64_t GetSize() const override { return m_buffer.GetSize(); }
        void Write(const void* data, std::uint64_t size, std::uint64_t offset) override { m_buffer.Write(data, size, offset); }
        bool IsMapped() const { return m_buffer.GetMappedData() != nullptr; }
        void Read(void* data, std::uint64_t size, std::uint64_t offset) const override { m_buffer.Read(data, size, offset); }
        VkBuffer GetHandle() const { return m_buffer.GetHandle(); }

        private:
        Vk::Buffer m_buffer;
    };

    class RhiStagingBuffer final : public RHI::StagingBuffer {
        public:
        explicit RhiStagingBuffer(Vk::Buffer buffer) : m_buffer(std::move(buffer)) {}
        std::uint64_t GetSize() const override { return m_buffer.GetSize(); }
        void Write(std::uint64_t offset, const void* data, std::uint64_t size) override { m_buffer.Write(data, size, offset); }
        VkBuffer GetHandle() const { return m_buffer.GetHandle(); }
        Vk::Buffer Take() { return std::move(m_buffer); }

        private:
        Vk::Buffer m_buffer;
    };
}
