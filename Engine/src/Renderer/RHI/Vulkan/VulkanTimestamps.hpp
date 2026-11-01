#pragma once

#include "Flux/Renderer/RHI/RHITypes.hpp"
#include "Renderer/RHI/Vulkan/Vulkan.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Flux::Vk {
    class Device;

    // Heures du GPU (CommandList::WriteTimestamp) : une réserve de RHI::MaxTimestamps requêtes par image en vol,
    // relues quand le GPU a fini l'image. Facultatives : sans heures sur la file graphique, rien n'est noté.
    class Timestamps {
        public:
        explicit Timestamps(const Device& device);
        ~Timestamps();

        Timestamps(const Timestamps&) = delete;
        Timestamps& operator=(const Timestamps&) = delete;

        // VK_NULL_HANDLE : pas d'heures (le GPU ne les note pas sur cette file).
        VkQueryPool GetPool(std::uint32_t frame) const { return m_pools[frame]; }
        // Relit les cases écrites (bit i : case i) par l'image `frame`, que le GPU vient de finir : pas d'attente.
        void Read(std::uint32_t frame, std::uint32_t written);
        // Millisecondes depuis la case 0 ; négatif pour une case non écrite.
        std::span<const double> Get() const { return {m_values.data(), m_count}; }

        private:
        VkDevice m_device = VK_NULL_HANDLE;
        double m_period = 0.0; // nanosecondes par tic ; 0 : pas d'heures du GPU
        std::array<VkQueryPool, RHI::FramesInFlight> m_pools{};
        std::array<double, RHI::MaxTimestamps> m_values{};
        std::size_t m_count = 0;
    };
}
