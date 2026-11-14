#include "Renderer/RHI/Vulkan/VulkanTransfer.hpp"

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
    }

    std::expected<std::unique_ptr<Transfer>, std::string> Transfer::Create(const Setup& setup) {
        // Le constructeur est privé : pas de std::make_unique.
        std::unique_ptr<Transfer> transfer(new Transfer());
        transfer->m_device = setup.device;
        transfer->m_allocator = setup.allocator;
        transfer->m_graphics = setup.graphics;
        const std::uint32_t graphicsFamily = setup.graphics->GetFamily();
        transfer->m_separate = setup.transferQueue != VK_NULL_HANDLE && setup.transferFamily != graphicsFamily;
        transfer->m_families = {graphicsFamily, transfer->m_separate ? setup.transferFamily : graphicsFamily};

        auto queue = Queue::Create(setup.device, transfer->m_separate ? setup.transferQueue : setup.graphics->GetHandle(),
                                   transfer->m_families[1]);
        if (!queue) {
            return std::unexpected(queue.error());
        }
        transfer->m_transfer = std::move(*queue);
        auto transferCommands = CommandRecycler::Create(setup.device, transfer->m_transfer);
        auto graphicsCommands = CommandRecycler::Create(setup.device, *setup.graphics);
        if (!transferCommands || !graphicsCommands) {
            return std::unexpected("création des command pools d'envoi");
        }
        transfer->m_transferCommands = std::move(*transferCommands);
        transfer->m_graphicsCommands = std::move(*graphicsCommands);
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
        // Le GPU a tout fini (Device attend avant). Les recycleurs d'abord : ils lisent la file de transfert.
        m_transferCommands = {};
        m_graphicsCommands = {};
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
                Batch& batch = Current().batch;
                batch.ringStart = std::min(batch.ringStart, start);
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
        Current().batch.staging.push_back(std::move(*staging));
        return Staged{handle, 0};
    }

    bool Transfer::BeginBatch() {
        Batch& batch = Current().batch;
        m_asyncWritten = m_asyncWritten || m_async;
        return batch.commands != VK_NULL_HANDLE || BeginCommands(m_transferCommands, batch.commands);
    }

    bool Transfer::BeginGraphics() {
        Stream& stream = Current();
        return stream.graphics != VK_NULL_HANDLE || BeginCommands(m_graphicsCommands, stream.graphics);
    }

    bool Transfer::BeginCommands(CommandRecycler& recycler, VkCommandBuffer& commands) {
        commands = recycler.Begin();
        return commands != VK_NULL_HANDLE;
    }

    void Transfer::BeginAsync() {
        m_async = true;
        m_asyncWritten = false;
    }

    std::uint64_t Transfer::EndAsync() {
        Stream& stream = m_streams[1];
        if (stream.batch.commands != VK_NULL_HANDLE) {
            Flush(stream);
        }
        m_async = false;
        // Lots partis en route (FlushBytes) : le dernier soumis les suit tous.
        return std::exchange(m_asyncWritten, false) ? m_asyncSerial : 0;
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
        Batch& batch = Current().batch;
        if (inUse) {
            batch.graphicsWait = m_graphics->GetSubmitted();
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
            vkCmdPipelineBarrier2(batch.commands, &dependency);
        }
        const VkBufferCopy region{staged->offset, offset, size};
        vkCmdCopyBuffer(batch.commands, staged->buffer, target, 1, &region);
        batch.bytes += size;
        return batch.bytes < FlushBytes || Flush(Current());
    }

    bool Transfer::WriteImage(const ImageWrite& write, std::span<const std::uint8_t> pixels) {
        if (!BeginBatch()) {
            return false;
        }
        auto staged = Stage(pixels.data(), pixels.size(), ImageAlignment);
        if (!staged) {
            Log::Error("envoi d'une texture : {}", staged.error());
            return false;
        }
        return RecordImage(write, staged->buffer, staged->offset, pixels.size());
    }

    bool Transfer::WriteImage(const ImageWrite& write, VkBuffer source, VkDeviceSize sourceOffset) {
        VkDeviceSize bytes = 0; // pour FlushBytes : ce que les régions lisent, à peu près
        for (const VkBufferImageCopy& region : write.regions) {
            bytes += VkDeviceSize{region.imageExtent.width} * region.imageExtent.height * 4 * region.imageSubresource.layerCount;
        }
        return BeginBatch() && RecordImage(write, source, sourceOffset, bytes);
    }

    void Transfer::KeepUntilCopied(Buffer buffer) {
        // Les copies qui le lisent sont dans un lot soumis ou ouvert : rendu quand tous ceux-là sont finis. Pas de lot
        // à lui, qui ferait attendre à la prochaine image les copies asynchrones soumises avant.
        const std::uint64_t mark = GetMarks().transfer;
        if (!m_transfer.IsDone(mark)) {
            m_kept.push_back({mark, std::move(buffer)});
        }
    }

    bool Transfer::RecordImage(const ImageWrite& write, VkBuffer source, VkDeviceSize sourceOffset, VkDeviceSize bytes) {
        if (!BeginGraphics()) {
            return false;
        }
        Stream& stream = Current();
        const VkCommandBuffer copies = stream.batch.commands;
        const VkCommandBuffer graphics = stream.graphics;
        std::vector<VkBufferImageCopy> regions(write.regions.begin(), write.regions.end());
        for (VkBufferImageCopy& region : regions) {
            region.bufferOffset += sourceOffset;
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
        Barrier(copies, barrier);
        vkCmdCopyBufferToImage(copies, source, write.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
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
            Barrier(copies, barrier);
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
        Barrier(graphics, barrier);

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
                Barrier(graphics, barrier);
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
                vkCmdBlitImage(graphics, write.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, write.image,
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
        stream.batch.bytes += bytes;
        return stream.batch.bytes < FlushBytes || Flush(stream);
    }

    bool Transfer::Flush(Stream& stream) {
        Batch& batch = stream.batch;
        if (batch.commands == VK_NULL_HANDLE) {
            return true;
        }
        const bool async = &stream == &m_streams[1];
        bool success = Check(vkEndCommandBuffer(batch.commands), "vkEndCommandBuffer");
        // Partie graphique d'un lot asynchrone terminée avant la soumission : sans elle, ses images ne serviraient pas.
        if (async && stream.graphics != VK_NULL_HANDLE && !(success && Check(vkEndCommandBuffer(stream.graphics), "vkEndCommandBuffer"))) {
            m_graphicsCommands.Release(stream.graphics);
            stream.graphics = VK_NULL_HANDLE;
            success = false;
        }
        if (success) {
            const VkSemaphoreSubmitInfo wait = m_graphics->GetWait(batch.graphicsWait);
            VkCommandBufferSubmitInfo commandInfo{};
            commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            commandInfo.commandBuffer = batch.commands;
            success = Check(m_transfer.Submit({&wait, batch.graphicsWait > 0 ? 1u : 0u}, {&commandInfo, 1}), "vkQueueSubmit2 (envois)");
        }
        if (!success) {
            // Jamais soumis : ses envois sont perdus (message dans la console).
            vkDeviceWaitIdle(m_device);
            ReleaseBatch(batch);
            batch = {};
            UpdateRingTail();
            return false;
        }
        batch.value = m_transfer.GetSubmitted();
        m_transferCommands.Retire(std::exchange(batch.commands, VK_NULL_HANDLE), batch.value);
        if (async) {
            m_asyncPending.push_back({std::exchange(stream.graphics, VK_NULL_HANDLE), batch.value, ++m_asyncSerial});
        } else {
            m_graphicsNeeds = batch.value;
        }
        m_inFlight.push_back(std::move(batch));
        batch = {};
        return true;
    }

    Transfer::GraphicsPart Transfer::TakeGraphics() {
        Stream& stream = m_streams[0];
        Flush(stream);
        m_takenCommands.clear();
        m_takenAsync.clear();
        m_takenSync = VK_NULL_HANDLE;
        GraphicsPart part;
        part.transferValue = std::exchange(m_graphicsNeeds, 0);
        if (stream.graphics != VK_NULL_HANDLE) {
            if (Check(vkEndCommandBuffer(stream.graphics), "vkEndCommandBuffer")) {
                m_takenSync = stream.graphics;
                m_takenCommands.push_back(stream.graphics);
            } else {
                m_graphicsCommands.Release(stream.graphics);
            }
            stream.graphics = VK_NULL_HANDLE;
        }
        // Lots asynchrones dont les copies sont finies : l'attente du sémaphore est déjà satisfaite (elle ne sert plus
        // qu'à rendre les copies visibles), la soumission ne s'arrête pas.
        if (!m_asyncPending.empty()) {
            const std::uint64_t done = m_transfer.GetCompleted();
            while (!m_asyncPending.empty() && m_asyncPending.front().transferValue <= done) {
                const AsyncPart& ready = m_asyncPending.front();
                part.transferValue = std::max(part.transferValue, ready.transferValue);
                part.asyncSerial = ready.serial;
                if (ready.commands != VK_NULL_HANDLE) {
                    m_takenCommands.push_back(ready.commands);
                }
                m_takenAsync.push_back(ready);
                m_asyncPending.pop_front();
            }
        }
        part.commands = m_takenCommands;
        return part;
    }

    void Transfer::GraphicsSubmitted(GraphicsPart& part, bool submitted) {
        if (submitted) {
            for (VkCommandBuffer commands : part.commands) {
                m_graphicsCommands.Retire(commands, m_graphics->GetSubmitted());
            }
            m_asyncAcquired = std::max(m_asyncAcquired, part.asyncSerial);
        } else {
            // Les envois attendus sont perdus ; les asynchrones repartiront avec la soumission suivante.
            m_graphicsCommands.Release(m_takenSync);
            for (auto async = m_takenAsync.rbegin(); async != m_takenAsync.rend(); ++async) {
                m_asyncPending.push_front(*async);
            }
        }
        m_takenSync = VK_NULL_HANDLE;
        m_takenAsync.clear();
        part = {};
    }

    Transfer::Marks Transfer::GetMarks() const {
        // La prochaine soumission graphique emporte peut-être des commandes déjà enregistrées qui s'en servent ; les
        // lots ouverts prendront les valeurs transfert suivantes.
        Marks marks{m_graphics->GetSubmitted() + 1, m_transfer.GetSubmitted()};
        for (const Stream& stream : m_streams) {
            if (stream.batch.commands != VK_NULL_HANDLE) {
                ++marks.transfer;
            }
        }
        return marks;
    }

    bool Transfer::IsDone(const Marks& marks) const {
        return m_graphics->IsDone(marks.graphics) && m_transfer.IsDone(marks.transfer);
    }

    void Transfer::Collect() {
        if (!m_inFlight.empty() || !m_kept.empty()) {
            const std::uint64_t done = m_transfer.GetCompleted();
            while (!m_kept.empty() && m_kept.front().first <= done) {
                m_kept.pop_front();
            }
            bool released = false;
            while (!m_inFlight.empty() && m_inFlight.front().value <= done) {
                ReleaseBatch(m_inFlight.front());
                m_inFlight.pop_front();
                released = true;
            }
            if (released) {
                UpdateRingTail();
            }
        }
    }

    void Transfer::UpdateRingTail() {
        // Tout ce qui précède la plus ancienne place d'un lot pas fini est libre (places prises dans l'ordre).
        std::uint64_t tail = m_ringHead;
        for (const Batch& batch : m_inFlight) {
            tail = std::min(tail, batch.ringStart);
        }
        for (const Stream& stream : m_streams) {
            tail = std::min(tail, stream.batch.ringStart);
        }
        m_ringTail = std::max(m_ringTail, tail);
    }

    void Transfer::ReleaseBatch(Batch& batch) {
        // Lot jamais soumis (les soumis ont rendu leur command buffer à Flush).
        m_transferCommands.Release(std::exchange(batch.commands, VK_NULL_HANDLE));
        batch.staging.clear();
    }
}
