#pragma once

// Buffers du GPU et mémoire d'envoi.

#include "Flux/Renderer/RHI/RHITypes.hpp"

#include <cstdint>

namespace Flux::RHI {
    struct BufferDesc
    {
        std::uint64_t size = 0;
        BufferUsage usage = BufferUsage::Vertex;
        MemoryType memory = MemoryType::GpuOnly;
    };

    class Buffer {
        public:
        virtual ~Buffer() = default;
        virtual std::uint64_t GetSize() const = 0;
        // Buffer Upload seulement.
        virtual void Write(const void* data, std::uint64_t size, std::uint64_t offset = 0) = 0;
        // Buffer Readback seulement, quand le GPU a fini de l'écrire (après Device::Submit, ou BeginFrame revenu sur
        // l'image qui l'a écrit).
        virtual void Read(void* data, std::uint64_t size, std::uint64_t offset = 0) const = 0;
    };

    // Mémoire d'envoi (Device::CreateStagingBuffer) remplie par le CPU, sur n'importe quel fil, puis copiée telle
    // quelle dans une texture (Device::CreateTexture) : les pixels décodés sur un fil de chargement y vont directement,
    // le fil de l'image n'a plus rien à recopier.
    class StagingBuffer {
        public:
        virtual ~StagingBuffer() = default;
        virtual std::uint64_t GetSize() const = 0;
        // Pas pendant qu'une copie le lit (après Device::ReleaseStaging, plus rien).
        virtual void Write(std::uint64_t offset, const void* data, std::uint64_t size) = 0;
    };
}
