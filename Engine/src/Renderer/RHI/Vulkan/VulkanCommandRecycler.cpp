#include "Renderer/RHI/Vulkan/VulkanCommandRecycler.hpp"

#include "Renderer/RHI/Vulkan/VulkanQueue.hpp"

namespace Flux::Vk {
    std::expected<CommandRecycler, std::string> CommandRecycler::Create(VkDevice device, const Queue& queue) {
        // RESET_COMMAND_BUFFER : vkBeginCommandBuffer remet à zéro un command buffer déjà joué, sans toucher aux autres.
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = queue.GetFamily();
        CommandRecycler recycler;
        if (!Check(vkCreateCommandPool(device, &info, nullptr, &recycler.m_pool), "vkCreateCommandPool")) {
            return std::unexpected("création d'un command pool");
        }
        recycler.m_device = device;
        recycler.m_queue = &queue;
        return recycler;
    }

    CommandRecycler::~CommandRecycler() {
        Reset();
    }

    CommandRecycler::CommandRecycler(CommandRecycler&& other) noexcept
        : m_device(std::exchange(other.m_device, VK_NULL_HANDLE)),
          m_queue(std::exchange(other.m_queue, nullptr)),
          m_pool(std::exchange(other.m_pool, VK_NULL_HANDLE)),
          m_free(std::move(other.m_free)),
          m_pending(std::move(other.m_pending)) {}

    CommandRecycler& CommandRecycler::operator=(CommandRecycler&& other) noexcept {
        if (this != &other) {
            Reset();
            m_device = std::exchange(other.m_device, VK_NULL_HANDLE);
            m_queue = std::exchange(other.m_queue, nullptr);
            m_pool = std::exchange(other.m_pool, VK_NULL_HANDLE);
            m_free = std::move(other.m_free);
            m_pending = std::move(other.m_pending);
        }
        return *this;
    }

    void CommandRecycler::Reset() {
        // Le GPU a fini (Device attend avant) : le pool rend ses command buffers avec lui.
        if (m_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_pool, nullptr);
        }
        m_pool = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_queue = nullptr;
        m_free.clear();
        m_pending.clear();
    }

    VkCommandBuffer CommandRecycler::Begin() {
        if (!m_pending.empty()) {
            const std::uint64_t done = m_queue->GetCompleted();
            while (!m_pending.empty() && m_pending.front().second <= done) {
                m_free.push_back(m_pending.front().first);
                m_pending.pop_front();
            }
        }
        VkCommandBuffer commands = VK_NULL_HANDLE;
        if (!m_free.empty()) {
            commands = m_free.back();
            m_free.pop_back();
        } else {
            VkCommandBufferAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocateInfo.commandPool = m_pool;
            allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocateInfo.commandBufferCount = 1;
            if (!Check(vkAllocateCommandBuffers(m_device, &allocateInfo, &commands), "vkAllocateCommandBuffers")) {
                return VK_NULL_HANDLE;
            }
        }
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!Check(vkBeginCommandBuffer(commands, &beginInfo), "vkBeginCommandBuffer")) {
            m_free.push_back(commands);
            return VK_NULL_HANDLE;
        }
        return commands;
    }

    void CommandRecycler::Retire(VkCommandBuffer commands, std::uint64_t value) {
        if (commands != VK_NULL_HANDLE) {
            m_pending.emplace_back(commands, value);
        }
    }

    void CommandRecycler::Release(VkCommandBuffer commands) {
        if (commands != VK_NULL_HANDLE) {
            // Peut-être encore en enregistrement (échec en route) : vkBeginCommandBuffer le refuserait dans cet état.
            vkResetCommandBuffer(commands, 0);
            m_free.push_back(commands);
        }
    }
}
