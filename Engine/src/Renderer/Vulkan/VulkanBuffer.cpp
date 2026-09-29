#include "Renderer/Vulkan/VulkanBuffer.hpp"

#include <cstring>
#include <utility>

namespace Flux::Vk {
    std::expected<Buffer, std::string> Buffer::Create(VmaAllocator allocator, VkDeviceSize size,
                                                      VkBufferUsageFlags usage, Memory memory) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        if (memory == Memory::Upload) {
            allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                 | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        } else {
            allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        }

        Buffer buffer;
        VmaAllocationInfo allocated{};
        if (!Check(vmaCreateBuffer(allocator, &bufferInfo, &allocationInfo,
                                   &buffer.m_buffer, &buffer.m_allocation, &allocated), "vmaCreateBuffer")) {
            return std::unexpected("allocation d'un buffer de " + std::to_string(size) + " octets");
        }
        buffer.m_allocator = allocator;
        buffer.m_size = size;
        buffer.m_mapped = allocated.pMappedData;
        return buffer;
    }

    void Buffer::Write(const void* data, VkDeviceSize size, VkDeviceSize offset) {
        std::memcpy(static_cast<std::byte*>(m_mapped) + offset, data, size);
        vmaFlushAllocation(m_allocator, m_allocation, offset, size);
    }

    Buffer::~Buffer() {
        Reset();
    }

    Buffer::Buffer(Buffer&& other) noexcept
        : m_allocator(std::exchange(other.m_allocator, VK_NULL_HANDLE)),
          m_buffer(std::exchange(other.m_buffer, VK_NULL_HANDLE)),
          m_allocation(std::exchange(other.m_allocation, VK_NULL_HANDLE)),
          m_size(std::exchange(other.m_size, 0)),
          m_mapped(std::exchange(other.m_mapped, nullptr)) {}

    Buffer& Buffer::operator=(Buffer&& other) noexcept {
        if (this != &other) {
            Reset();
            m_allocator = std::exchange(other.m_allocator, VK_NULL_HANDLE);
            m_buffer = std::exchange(other.m_buffer, VK_NULL_HANDLE);
            m_allocation = std::exchange(other.m_allocation, VK_NULL_HANDLE);
            m_size = std::exchange(other.m_size, 0);
            m_mapped = std::exchange(other.m_mapped, nullptr);
        }
        return *this;
    }

    void Buffer::Reset() {
        if (m_buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
        }
        m_buffer = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
        m_mapped = nullptr;
        m_size = 0;
    }
}
