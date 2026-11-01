#include "Renderer/Vulkan/VulkanFrames.hpp"

#include "Renderer/Vulkan/VulkanDevice.hpp"

namespace Flux::Vk {
    Frames::~Frames() {
        const VkDevice device = m_device.GetHandle();
        for (Frame& frame : m_frames) {
            vkDestroyFence(device, frame.inFlight, nullptr);
            vkDestroySemaphore(device, frame.imageAvailable, nullptr);
            vkDestroyCommandPool(device, frame.commandPool, nullptr);
        }
    }

    bool Frames::Create() {
        const VkDevice device = m_device.GetHandle();
        for (Frame& frame : m_frames) {
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolInfo.queueFamilyIndex = m_device.GetGraphicsQueueFamily();
            if (!Check(vkCreateCommandPool(device, &poolInfo, nullptr, &frame.commandPool), "vkCreateCommandPool")) {
                return false;
            }

            VkCommandBufferAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocateInfo.commandPool = frame.commandPool;
            allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocateInfo.commandBufferCount = 1;
            if (!Check(vkAllocateCommandBuffers(device, &allocateInfo, &frame.commandBuffer), "vkAllocateCommandBuffers")) {
                return false;
            }

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
        const Frame& frame = m_frames[index];
        vkResetFences(m_device.GetHandle(), 1, &frame.inFlight);
        vkResetCommandPool(m_device.GetHandle(), frame.commandPool, 0);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
    }
}
