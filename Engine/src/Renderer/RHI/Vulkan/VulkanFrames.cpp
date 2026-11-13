#include "Renderer/RHI/Vulkan/VulkanFrames.hpp"

#include "Renderer/RHI/Vulkan/VulkanDevice.hpp"
#include "Renderer/RHI/Vulkan/VulkanTransfer.hpp"

namespace Flux::Vk {
    Frames::~Frames() {
        const VkDevice device = m_device.GetHandle();
        for (Frame& frame : m_frames) {
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
        }
        return true;
    }

    VkResult Frames::Wait(std::uint32_t index) const {
        return m_device.GetTransfer().WaitGraphics(m_frames[index].graphicsValue);
    }
}
