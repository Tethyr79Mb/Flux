#include "Renderer/RHI/Vulkan/VulkanFrames.hpp"

#include "Renderer/RHI/Vulkan/VulkanDevice.hpp"

namespace Flux::Vk {
    Frames::~Frames() {
        const VkDevice device = m_device.GetHandle();
        for (Frame& frame : m_frames) {
            vkDestroyFence(device, frame.inFlight, nullptr);
            vkDestroySemaphore(device, frame.imageAvailable, nullptr);
        }
    }

    bool Frames::Create() {
        const VkDevice device = m_device.GetHandle();
        for (Frame& frame : m_frames) {
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (!Check(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.imageAvailable), "vkCreateSemaphore")) {
                return false;
            }

            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (!Check(vkCreateFence(device, &fenceInfo, nullptr, &frame.inFlight), "vkCreateFence")) {
                return false;
            }
        }
        return true;
    }

    VkResult Frames::Wait(std::uint32_t index) const {
        return vkWaitForFences(m_device.GetHandle(), 1, &m_frames[index].inFlight, VK_TRUE, UINT64_MAX);
    }

    void Frames::Begin(std::uint32_t index) const {
        vkResetFences(m_device.GetHandle(), 1, &m_frames[index].inFlight);
    }
}
