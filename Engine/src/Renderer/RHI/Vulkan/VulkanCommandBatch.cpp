#include "Renderer/RHI/Vulkan/VulkanCommandBatch.hpp"

#include "Renderer/RHI/Vulkan/VulkanDevice.hpp"

namespace Flux::Vk {
    CommandBatch::~CommandBatch() {
        for (const std::vector<Slot>* slots : {&m_slots, &m_secondarySlots}) {
            for (const Slot& slot : *slots) {
                vkDestroyCommandPool(m_device.GetHandle(), slot.pool, nullptr);
            }
        }
    }

    bool CommandBatch::Reserve(std::vector<Slot>& slots, std::uint32_t count, VkCommandBufferLevel level) {
        const VkDevice device = m_device.GetHandle();
        while (slots.size() < count) {
            Slot slot;
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolInfo.queueFamilyIndex = m_device.GetGraphicsQueueFamily();
            if (!Check(vkCreateCommandPool(device, &poolInfo, nullptr, &slot.pool), "vkCreateCommandPool")) {
                return false;
            }
            VkCommandBufferAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocateInfo.commandPool = slot.pool;
            allocateInfo.level = level;
            allocateInfo.commandBufferCount = 1;
            if (!Check(vkAllocateCommandBuffers(device, &allocateInfo, &slot.buffer), "vkAllocateCommandBuffers")) {
                vkDestroyCommandPool(device, slot.pool, nullptr);
                return false;
            }
            slots.push_back(slot);
        }
        return true;
    }

    VkCommandBuffer CommandBatch::Open() {
        const VkCommandBuffer buffer = m_slots[m_used++].buffer;
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(buffer, &beginInfo);
        m_order.push_back(buffer);
        return buffer;
    }

    VkCommandBuffer CommandBatch::Begin() {
        for (std::uint32_t i = 0; i < m_used; ++i) {
            vkResetCommandPool(m_device.GetHandle(), m_slots[i].pool, 0);
        }
        m_used = 0;
        m_listCount = 0;
        m_order.clear();
        for (std::uint32_t i = 0; i < m_secondaryUsed; ++i) {
            vkResetCommandPool(m_device.GetHandle(), m_secondarySlots[i].pool, 0);
        }
        m_secondaryUsed = 0;
        if (!Reserve(m_slots, 1, VK_COMMAND_BUFFER_LEVEL_PRIMARY)) {
            return VK_NULL_HANDLE;
        }
        // Le plus grand lot possible, une fois : plus d'allocation ensuite.
        m_order.reserve(2 * RHI::MaxParallelCommandLists + 1);
        return Open();
    }

    std::span<RHI::CommandList* const> CommandBatch::Fork(RhiCommandList& main, std::uint32_t count) {
        if (count == 0 || m_listCount + count > RHI::MaxParallelCommandLists || !Reserve(m_slots, m_used + count + 1, VK_COMMAND_BUFFER_LEVEL_PRIMARY)) {
            return {};
        }
        // Une étape ouverte finirait après les listes parallèles : fermée ici, la suivante la remplace.
        main.EndStep();
        if (!Check(vkEndCommandBuffer(main.GetHandle()), "vkEndCommandBuffer")) {
            return {};
        }
        const std::uint32_t first = m_listCount;
        for (std::uint32_t i = 0; i < count; ++i) {
            RhiCommandList& list = m_lists[m_listCount];
            list.BeginParallel(Open(), main);
            m_listPointers[m_listCount++] = &list;
        }
        main.ContinueIn(Open());
        return {m_listPointers.data() + first, count};
    }

    std::span<RHI::CommandList* const> CommandBatch::OpenSecondaries(const RhiCommandList& owner,
                                                                    const VkCommandBufferInheritanceRenderingInfo& rendering,
                                                                    std::uint32_t count) {
        const std::scoped_lock lock(m_secondaryMutex);
        if (count == 0 || m_secondaryUsed + count > RHI::MaxSecondaryCommandLists
            || !Reserve(m_secondarySlots, m_secondaryUsed + count, VK_COMMAND_BUFFER_LEVEL_SECONDARY)) {
            return {};
        }
        VkCommandBufferInheritanceInfo inheritance{};
        inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
        inheritance.pNext = &rendering;
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        beginInfo.pInheritanceInfo = &inheritance;
        const std::uint32_t first = m_secondaryUsed;
        for (std::uint32_t i = first; i < first + count; ++i) {
            if (!Check(vkBeginCommandBuffer(m_secondarySlots[i].buffer, &beginInfo), "vkBeginCommandBuffer")) {
                m_secondaryUsed = first + count; // ceux déjà ouverts sont vidés au prochain Begin
                return {};
            }
            m_secondaryLists[i].BeginSecondary(m_secondarySlots[i].buffer, owner);
            m_secondaryPointers[i] = &m_secondaryLists[i];
        }
        m_secondaryUsed += count;
        return {m_secondaryPointers.data() + first, count};
    }

    bool CommandBatch::End(RhiCommandList& main) {
        bool ended = true;
        for (std::uint32_t i = 0; i < m_listCount; ++i) {
            m_lists[i].EndStep();
            ended = Check(vkEndCommandBuffer(m_lists[i].GetHandle()), "vkEndCommandBuffer") && ended;
        }
        return Check(vkEndCommandBuffer(main.GetHandle()), "vkEndCommandBuffer") && ended;
    }

    std::uint32_t CommandBatch::GetWrittenTimestamps(const RhiCommandList& main) const {
        std::uint32_t written = main.GetWrittenTimestamps();
        for (std::uint32_t i = 0; i < m_listCount; ++i) {
            written |= m_lists[i].GetWrittenTimestamps();
        }
        return written;
    }
}
