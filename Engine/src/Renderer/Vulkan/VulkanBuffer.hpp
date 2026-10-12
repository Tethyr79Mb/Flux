#pragma once

#include "Renderer/Vulkan/Vulkan.hpp"

#include <expected>
#include <string>

namespace Flux::Vk {
    // Buffer GPU alloué par VMA, détruit automatiquement.
    class Buffer {
        public:
        enum class Memory {
            GpuOnly, // mémoire vidéo, remplie par copie depuis un buffer Upload
            Upload,  // visible et mappée en permanence pour le CPU, écrite séquentiellement
            // Comme Upload, pour une copie unique (buffer intermédiaire) : sa propre allocation, rendue au pilote dès
            // sa destruction. Dans un bloc partagé, VMA garderait le bloc vide (256 Mo de RAM après un gros envoi).
            Staging,
            Readback, // visible et mappée en permanence pour le CPU, écrite par le GPU et relue par le CPU
        };

        static std::expected<Buffer, std::string> Create(VmaAllocator allocator, VkDeviceSize size,
                                                         VkBufferUsageFlags usage, Memory memory);

        Buffer() = default;
        ~Buffer();

        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;
        Buffer(Buffer&& other) noexcept;
        Buffer& operator=(Buffer&& other) noexcept;

        VkBuffer GetHandle() const { return m_buffer; }
        VkDeviceSize GetSize() const { return m_size; }

        // nullptr pour un buffer GpuOnly.
        void* GetMappedData() const { return m_mapped; }

        // Copie dans un buffer Upload puis flush (sans effet si la mémoire est cohérente, cas courant sur PC).
        void Write(const void* data, VkDeviceSize size, VkDeviceSize offset = 0);
        // Relit un buffer Readback (invalide d'abord le cache du CPU si la mémoire n'est pas cohérente).
        void Read(void* data, VkDeviceSize size, VkDeviceSize offset = 0) const;

        private:
        void Reset();

        VmaAllocator m_allocator = VK_NULL_HANDLE;
        VkBuffer m_buffer = VK_NULL_HANDLE;
        VmaAllocation m_allocation = VK_NULL_HANDLE;
        VkDeviceSize m_size = 0;
        void* m_mapped = nullptr;
    };
}
