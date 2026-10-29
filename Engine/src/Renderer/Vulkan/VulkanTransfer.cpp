#include "Renderer/Vulkan/VulkanTransfer.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace Flux::Vk {
    namespace {
        // Anneau d'envoi : de quoi tenir les envois d'une image chargée d'un coup (modèle et ses textures) sans
        // allocation. Un envoi de plus de la moitié a son propre buffer, rendu dès que la copie est finie.
        constexpr VkDeviceSize RingSize = VkDeviceSize{64} << 20;
        // Au-delà, le lot part tout de suite : la copie commence pendant que le CPU prépare la suite.
        constexpr VkDeviceSize FlushBytes = VkDeviceSize{32} << 20;
        constexpr VkDeviceSize BufferAlignment = 16;
        // Copie buffer -> image : multiple de la taille d'un bloc (16 octets en BC7) et de
        // optimalBufferCopyOffsetAlignment (au plus 256 sur les GPU de bureau).
        constexpr VkDeviceSize ImageAlignment = 256;

        void Barrier(VkCommandBuffer commands, const VkImageMemoryBarrier2& image) {
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &image;
            vkCmdPipelineBarrier2(commands, &dependency);
        }

        bool CreateTimeline(VkDevice device, VkSemaphore& semaphore) {
            VkSemaphoreTypeCreateInfo type{};
            type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            info.pNext = &type;
            return Check(vkCreateSemaphore(device, &info, nullptr, &semaphore), "vkCreateSemaphore");
        }

        bool CreatePool(VkDevice device, std::uint32_t family, VkCommandPool& pool) {
            VkCommandPoolCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            info.queueFamilyIndex = family;
            return Check(vkCreateCommandPool(device, &info, nullptr, &pool), "vkCreateCommandPool");
        }

        bool BeginCommands(VkDevice device, VkCommandPool pool, VkCommandBuffer& commands) {
            VkCommandBufferAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocateInfo.commandPool = pool;
            allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocateInfo.commandBufferCount = 1;
            if (!Check(vkAllocateCommandBuffers(device, &allocateInfo, &commands), "vkAllocateCommandBuffers")) {
                commands = VK_NULL_HANDLE;
                return false;
            }
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(commands, &beginInfo);
            return true;
        }

        std::uint64_t CounterValue(VkDevice device, VkSemaphore semaphore) {
            std::uint64_t value = 0;
            vkGetSemaphoreCounterValue(device, semaphore, &value);
            return value;
        }
    }

    std::expected<std::unique_ptr<Transfer>, std::string> Transfer::Create(const Setup& setup) {
        // Le constructeur est privé : pas de std::make_unique.
        std::unique_ptr<Transfer> transfer(new Transfer());
        transfer->m_device = setup.device;
        transfer->m_allocator = setup.allocator;
        transfer->m_graphicsQueue = setup.graphicsQueue;
        transfer->m_separate = setup.transferQueue != VK_NULL_HANDLE && setup.transferFamily != setup.graphicsFamily;
        transfer->m_transferQueue = transfer->m_separate ? setup.transferQueue : setup.graphicsQueue;
        transfer->m_families = {setup.graphicsFamily, transfer->m_separate ? setup.transferFamily : setup.graphicsFamily};

        if (!CreatePool(setup.device, transfer->m_families[1], transfer->m_transferPool)
            || !CreatePool(setup.device, transfer->m_families[0], transfer->m_graphicsPool)) {
            return std::unexpected("création des command pools d'envoi");
        }
        if (!CreateTimeline(setup.device, transfer->m_transferTimeline) || !CreateTimeline(setup.device, transfer->m_graphicsTimeline)) {
            return std::unexpected("création des sémaphores d'envoi");
        }
        auto ring = Buffer::Create(setup.allocator, RingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Memory::Staging);
        if (!ring) {
            return std::unexpected(ring.error());
        }
        transfer->m_ring = std::move(*ring);
        if (transfer->m_separate) {
            Log::Info("Envois vers le GPU : file de transfert dédiée (famille {})", setup.transferFamily);
        } else {
            Log::Info("Envois vers le GPU : file graphique (pas de file de transfert séparée)");
        }
        return transfer;
    }

    Transfer::~Transfer() {
        // Le GPU a tout fini (Device attend avant) : les pools rendent leurs command buffers avec eux.
        if (m_transferPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_transferPool, nullptr);
        }
        if (m_graphicsPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_graphicsPool, nullptr);
        }
        if (m_transferTimeline != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, m_transferTimeline, nullptr);
        }
        if (m_graphicsTimeline != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, m_graphicsTimeline, nullptr);
        }
    }

    std::span<const std::uint32_t> Transfer::GetSharedFamilies() const {
        if (!m_separate) {
            return {};
        }
        return m_families;
    }

    std::expected<Transfer::Staged, std::string> Transfer::Stage(const void* data, VkDeviceSize size, VkDeviceSize alignment) {
        if (size <= RingSize / 2) {
            Collect();
            std::uint64_t start = (m_ringHead + alignment - 1) / alignment * alignment;
            VkDeviceSize position = start % RingSize;
            if (position + size > RingSize) {
                start += RingSize - position; // pas de place avant la fin : on repart du début
                position = 0;
            }
            if (start + size - m_ringTail <= RingSize) {
                m_ringHead = start + size;
                m_ring.Write(data, size, position);
                return Staged{m_ring.GetHandle(), position};
            }
            // Anneau plein de copies pas encore finies : un buffer à part plutôt qu'attendre le GPU.
        }
        auto staging = Buffer::Create(m_allocator, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Memory::Staging);
        if (!staging) {
            return std::unexpected(staging.error());
        }
        staging->Write(data, size);
        const VkBuffer handle = staging->GetHandle();
        m_batch.staging.push_back(std::move(*staging));
        return Staged{handle, 0};
    }

    bool Transfer::BeginBatch() {
        return m_batch.commands != VK_NULL_HANDLE || BeginCommands(m_device, m_transferPool, m_batch.commands);
    }

    bool Transfer::BeginGraphics() {
        return m_graphicsCommands != VK_NULL_HANDLE || BeginCommands(m_device, m_graphicsPool, m_graphicsCommands);
    }

    bool Transfer::WriteBuffer(VkBuffer target, VkDeviceSize offset, const void* data, VkDeviceSize size, bool inUse) {
        if (size == 0) {
            return true;
        }
        if (!BeginBatch()) {
            return false;
        }
        auto staged = Stage(data, size, BufferAlignment);
        if (!staged) {
            Log::Error("envoi d'un buffer : {}", staged.error());
            return false;
        }
        if (inUse) {
            m_batch.graphicsWait = m_graphicsValue;
            // Deux envois dans le même buffer peuvent se recouvrir : le second après le premier.
            VkMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(m_batch.commands, &dependency);
        }
        const VkBufferCopy region{staged->offset, offset, size};
        vkCmdCopyBuffer(m_batch.commands, staged->buffer, target, 1, &region);
        m_batch.bytes += size;
        return m_batch.bytes < FlushBytes || Flush();
    }

    bool Transfer::WriteImage(const ImageWrite& write, std::span<const std::uint8_t> pixels) {
        if (!BeginBatch() || !BeginGraphics()) {
            return false;
        }
        auto staged = Stage(pixels.data(), pixels.size(), ImageAlignment);
        if (!staged) {
            Log::Error("envoi d'une texture : {}", staged.error());
            return false;
        }
        std::vector<VkBufferImageCopy> regions(write.regions.begin(), write.regions.end());
        for (VkBufferImageCopy& region : regions) {
            region.bufferOffset += staged->offset;
        }

        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = write.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, write.levels, 0, write.layers};

        // Image neuve : la file de transfert la prend sans transfert de propriété (contenu indéfini, tout réécrit).
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        Barrier(m_batch.commands, barrier);
        vkCmdCopyBufferToImage(m_batch.commands, staged->buffer, write.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(regions.size()), regions.data());

        // Passage à la file graphique : libération ici, acquisition là-bas, avec les mêmes dispositions (la transition
        // n'a lieu qu'une fois). Avec une seule file, la seconde barrière suffit.
        const VkImageLayout handedLayout = write.generateMips ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                                              : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = handedLayout;
        if (m_separate) {
            barrier.srcQueueFamilyIndex = m_families[1];
            barrier.dstQueueFamilyIndex = m_families[0];
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.dstAccessMask = VK_ACCESS_2_NONE;
            Barrier(m_batch.commands, barrier);
        }
        // Côté graphique : après l'attente du sémaphore (toutes étapes), à laquelle ALL_TRANSFER s'enchaîne.
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = m_separate ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_WRITE_BIT;
        if (write.generateMips) {
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        } else {
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        }
        Barrier(m_graphicsCommands, barrier);

        if (write.generateMips) {
            // Chaque niveau est une réduction de moitié du précédent, qui passe ensuite en lecture.
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            const auto transition = [&](std::uint32_t level, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                                        VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
                barrier.srcStageMask = srcStage;
                barrier.srcAccessMask = srcAccess;
                barrier.dstStageMask = dstStage;
                barrier.dstAccessMask = dstAccess;
                barrier.oldLayout = from;
                barrier.newLayout = to;
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, write.layers};
                Barrier(m_graphicsCommands, barrier);
            };
            std::int32_t width = static_cast<std::int32_t>(write.extent.width);
            std::int32_t height = static_cast<std::int32_t>(write.extent.height);
            for (std::uint32_t level = 1; level < write.levels; ++level) {
                transition(level - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                const std::int32_t nextWidth = std::max(width / 2, 1);
                const std::int32_t nextHeight = std::max(height / 2, 1);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, write.layers};
                blit.srcOffsets[1] = {width, height, 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, write.layers};
                blit.dstOffsets[1] = {nextWidth, nextHeight, 1};
                vkCmdBlitImage(m_graphicsCommands, write.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, write.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                transition(level - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                width = nextWidth;
                height = nextHeight;
            }
            // Le dernier niveau n'a servi que de destination.
            transition(write.levels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        m_batch.bytes += pixels.size();
        return m_batch.bytes < FlushBytes || Flush();
    }

    bool Transfer::Flush() {
        if (m_batch.commands == VK_NULL_HANDLE) {
            return true;
        }
        bool success = Check(vkEndCommandBuffer(m_batch.commands), "vkEndCommandBuffer");
        if (success) {
            VkSemaphoreSubmitInfo wait{};
            wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            wait.semaphore = m_graphicsTimeline;
            wait.value = m_batch.graphicsWait;
            wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkSemaphoreSubmitInfo signal{};
            signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            signal.semaphore = m_transferTimeline;
            signal.value = m_transferValue + 1;
            signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkCommandBufferSubmitInfo commandInfo{};
            commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            commandInfo.commandBuffer = m_batch.commands;
            VkSubmitInfo2 submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            submitInfo.waitSemaphoreInfoCount = m_batch.graphicsWait > 0 ? 1 : 0;
            submitInfo.pWaitSemaphoreInfos = &wait;
            submitInfo.commandBufferInfoCount = 1;
            submitInfo.pCommandBufferInfos = &commandInfo;
            submitInfo.signalSemaphoreInfoCount = 1;
            submitInfo.pSignalSemaphoreInfos = &signal;
            success = Check(vkQueueSubmit2(m_transferQueue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit2 (envois)");
        }
        if (!success) {
            // Jamais soumis : ses envois sont perdus (message dans la console).
            vkDeviceWaitIdle(m_device);
            ReleaseBatch(m_batch);
            m_batch = {};
            if (m_inFlight.empty()) {
                m_ringTail = m_ringHead;
            }
            return false;
        }
        m_batch.value = ++m_transferValue;
        m_batch.ringEnd = m_ringHead;
        m_graphicsNeeds = m_transferValue;
        m_inFlight.push_back(std::move(m_batch));
        m_batch = {};
        return true;
    }

    Transfer::GraphicsPart Transfer::TakeGraphics() {
        Flush();
        GraphicsPart part;
        part.transferValue = std::exchange(m_graphicsNeeds, 0);
        if (m_graphicsCommands != VK_NULL_HANDLE) {
            if (Check(vkEndCommandBuffer(m_graphicsCommands), "vkEndCommandBuffer")) {
                part.commands = m_graphicsCommands;
            } else {
                vkFreeCommandBuffers(m_device, m_graphicsPool, 1, &m_graphicsCommands);
            }
            m_graphicsCommands = VK_NULL_HANDLE;
        }
        return part;
    }

    VkSemaphoreSubmitInfo Transfer::GetWait(const GraphicsPart& part) const {
        VkSemaphoreSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = m_transferTimeline;
        wait.value = part.transferValue;
        wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        return wait;
    }

    VkSemaphoreSubmitInfo Transfer::GetSignal() const {
        VkSemaphoreSubmitInfo signal{};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = m_graphicsTimeline;
        signal.value = m_graphicsValue + 1;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        return signal;
    }

    void Transfer::GraphicsSubmitted(GraphicsPart& part, bool submitted) {
        if (submitted) {
            ++m_graphicsValue;
            if (part.commands != VK_NULL_HANDLE) {
                m_graphicsInFlight.push_back({part.commands, m_graphicsValue});
            }
        } else if (part.commands != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(m_device, m_graphicsPool, 1, &part.commands);
        }
        part = {};
    }

    bool Transfer::WaitGraphics() const {
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &m_graphicsTimeline;
        waitInfo.pValues = &m_graphicsValue;
        return Check(vkWaitSemaphores(m_device, &waitInfo, UINT64_MAX), "vkWaitSemaphores");
    }

    void Transfer::Collect() {
        if (!m_inFlight.empty()) {
            const std::uint64_t done = CounterValue(m_device, m_transferTimeline);
            while (!m_inFlight.empty() && m_inFlight.front().value <= done) {
                m_ringTail = m_inFlight.front().ringEnd;
                ReleaseBatch(m_inFlight.front());
                m_inFlight.pop_front();
            }
        }
        if (!m_graphicsInFlight.empty()) {
            const std::uint64_t done = CounterValue(m_device, m_graphicsTimeline);
            while (!m_graphicsInFlight.empty() && m_graphicsInFlight.front().value <= done) {
                vkFreeCommandBuffers(m_device, m_graphicsPool, 1, &m_graphicsInFlight.front().commands);
                m_graphicsInFlight.pop_front();
            }
        }
    }

    void Transfer::ReleaseBatch(Batch& batch) {
        if (batch.commands != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(m_device, m_transferPool, 1, &batch.commands);
            batch.commands = VK_NULL_HANDLE;
        }
        batch.staging.clear();
    }
}
