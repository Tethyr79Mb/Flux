#include "Renderer/RHI/Vulkan/VulkanQueue.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <utility>

namespace Flux::Vk {
    std::expected<Queue, std::string> Queue::Create(VkDevice device, VkQueue queue, std::uint32_t family) {
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        info.pNext = &type;
        Queue result;
        if (!Check(vkCreateSemaphore(device, &info, nullptr, &result.m_timeline), "vkCreateSemaphore")) {
            return std::unexpected("création du sémaphore timeline d'une file");
        }
        result.m_device = device;
        result.m_queue = queue;
        result.m_family = family;
        return result;
    }

    Queue::~Queue() {
        Reset();
    }

    Queue::Queue(Queue&& other) noexcept
        : m_device(std::exchange(other.m_device, VK_NULL_HANDLE)),
          m_queue(std::exchange(other.m_queue, VK_NULL_HANDLE)),
          m_family(other.m_family),
          m_timeline(std::exchange(other.m_timeline, VK_NULL_HANDLE)),
          m_submitted(std::exchange(other.m_submitted, 0)) {}

    Queue& Queue::operator=(Queue&& other) noexcept {
        if (this != &other) {
            Reset();
            m_device = std::exchange(other.m_device, VK_NULL_HANDLE);
            m_queue = std::exchange(other.m_queue, VK_NULL_HANDLE);
            m_family = other.m_family;
            m_timeline = std::exchange(other.m_timeline, VK_NULL_HANDLE);
            m_submitted = std::exchange(other.m_submitted, 0);
        }
        return *this;
    }

    void Queue::Reset() {
        // Le GPU a fini (Device attend avant) : plus rien n'attend ni ne signale le timeline.
        if (m_timeline != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, m_timeline, nullptr);
        }
        m_timeline = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_queue = VK_NULL_HANDLE;
    }

    VkResult Queue::Submit(std::span<const VkSemaphoreSubmitInfo> waits, std::span<const VkCommandBufferSubmitInfo> commands,
                           std::span<const VkSemaphoreSubmitInfo> signals) {
        assert(signals.size() <= MaxSignals);
        std::array<VkSemaphoreSubmitInfo, MaxSignals + 1> allSignals{};
        std::size_t count = 0;
        for (const VkSemaphoreSubmitInfo& signal : signals.first(std::min(signals.size(), MaxSignals))) {
            allSignals[count++] = signal;
        }
        VkSemaphoreSubmitInfo& own = allSignals[count++];
        own.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        own.semaphore = m_timeline;
        own.value = m_submitted + 1;
        own.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkSubmitInfo2 submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submitInfo.waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size());
        submitInfo.pWaitSemaphoreInfos = waits.data();
        submitInfo.commandBufferInfoCount = static_cast<std::uint32_t>(commands.size());
        submitInfo.pCommandBufferInfos = commands.data();
        submitInfo.signalSemaphoreInfoCount = static_cast<std::uint32_t>(count);
        submitInfo.pSignalSemaphoreInfos = allSignals.data();
        const VkResult result = vkQueueSubmit2(m_queue, 1, &submitInfo, VK_NULL_HANDLE);
        if (result == VK_SUCCESS) {
            ++m_submitted;
        }
        return result;
    }

    VkSemaphoreSubmitInfo Queue::GetWait(std::uint64_t value) const {
        VkSemaphoreSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = m_timeline;
        wait.value = value;
        wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        return wait;
    }

    std::uint64_t Queue::GetCompleted() const {
        std::uint64_t value = 0;
        vkGetSemaphoreCounterValue(m_device, m_timeline, &value);
        return value;
    }

    VkResult Queue::Wait(std::uint64_t value) const {
        if (value == 0) {
            return VK_SUCCESS;
        }
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &m_timeline;
        waitInfo.pValues = &value;
        return vkWaitSemaphores(m_device, &waitInfo, UINT64_MAX);
    }
}
