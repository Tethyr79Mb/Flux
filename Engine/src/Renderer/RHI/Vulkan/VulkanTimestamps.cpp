#include "Renderer/RHI/Vulkan/VulkanTimestamps.hpp"

#include "Renderer/RHI/Vulkan/VulkanDevice.hpp"

namespace Flux::Vk {
    Timestamps::Timestamps(const Device& device) : m_device(device.GetHandle()), m_period(device.GetTimestampPeriod()) {
        if (m_period <= 0.0) {
            return;
        }
        for (VkQueryPool& pool : m_pools) {
            VkQueryPoolCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = RHI::MaxTimestamps;
            if (vkCreateQueryPool(m_device, &info, nullptr, &pool) != VK_SUCCESS) {
                pool = VK_NULL_HANDLE; // l'image se dessine sans
            }
        }
    }

    Timestamps::~Timestamps() {
        for (VkQueryPool pool : m_pools) {
            vkDestroyQueryPool(m_device, pool, nullptr);
        }
    }

    void Timestamps::Read(std::uint32_t frame, std::uint32_t written) {
        m_count = 0;
        const VkQueryPool pool = m_pools[frame];
        if (pool == VK_NULL_HANDLE || written == 0) {
            return;
        }
        // Valeur et disponibilité par case ; le GPU a fini l'image (barrière attendue) : pas d'attente ici.
        std::array<std::uint64_t, 2 * RHI::MaxTimestamps> values{};
        const VkResult result = vkGetQueryPoolResults(m_device, pool, 0, RHI::MaxTimestamps, sizeof(values), values.data(),
                                                      2 * sizeof(std::uint64_t),
                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result != VK_SUCCESS && result != VK_NOT_READY) {
            return;
        }
        const bool firstReady = (written & 1u) != 0 && values[1] != 0;
        for (std::uint32_t slot = 0; slot < RHI::MaxTimestamps; ++slot) {
            const bool ready = firstReady && (written & (1u << slot)) != 0 && values[2 * slot + 1] != 0;
            m_values[slot] = ready ? static_cast<double>(values[2 * slot] - values[0]) * m_period * 1.0e-6 : -1.0;
            if (ready) {
                m_count = slot + 1;
            }
        }
    }
}
