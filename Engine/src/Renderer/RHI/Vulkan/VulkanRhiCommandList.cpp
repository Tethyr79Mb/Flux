#include "Renderer/RHI/Vulkan/VulkanRhiCommandList.hpp"

#include "Renderer/RHI/Vulkan/VulkanCommandBatch.hpp"

#include "Renderer/RHI/Vulkan/VulkanAccelerationStructure.hpp"
#include "Renderer/RHI/Vulkan/VulkanRhiBindGroup.hpp"
#include "Renderer/RHI/Vulkan/VulkanRhiBuffer.hpp"
#include "Renderer/RHI/Vulkan/VulkanRhiPipeline.hpp"
#include "Renderer/RHI/Vulkan/VulkanRhiTexture.hpp"
#include "Renderer/RHI/Vulkan/VulkanTypes.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace Flux::Vk {
    void RhiCommandList::Begin(VkCommandBuffer commandBuffer, VkImage image, VkImageView view, VkExtent2D extent,
                               VkQueryPool queryPool, CommandBatch& batch) {
        m_commandBuffer = commandBuffer;
        m_batch = &batch;
        m_canFork = true;
        m_secondaries = {};
        m_image = image;
        m_view = view;
        m_extent = extent;
        m_layout = VK_NULL_HANDLE;
        m_bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        m_pushStages = 0;
        m_queryPool = queryPool;
        m_writtenTimestamps = 0;
        m_markers = {};
        m_openStep = RHI::MaxSteps;
        m_labelOpen = false;
        m_pendingImageCount = 0;
        m_memoryPending = false;
        if (m_queryPool != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(m_commandBuffer, m_queryPool, 0, RHI::MaxTimestamps);
        }
    }

    void RhiCommandList::BeginParallel(VkCommandBuffer commandBuffer, const RhiCommandList& main) {
        m_commandBuffer = commandBuffer;
        m_batch = main.m_batch;
        m_canFork = false;
        m_secondaries = {};
        m_image = main.m_image;
        m_view = main.m_view;
        m_extent = main.m_extent;
        m_queryPool = main.m_queryPool;
        m_markers = main.m_markers;
        m_debugLabels = main.m_debugLabels;
        m_meshShaders = main.m_meshShaders;
        m_writtenTimestamps = 0;
        m_openStep = RHI::MaxSteps;
        m_labelOpen = false;
        m_pendingImageCount = 0;
        m_memoryPending = false;
        ContinueIn(commandBuffer);
    }

    void RhiCommandList::BeginSecondary(VkCommandBuffer commandBuffer, const RhiCommandList& owner) {
        BeginParallel(commandBuffer, owner);
        m_batch = nullptr;
    }

    void RhiCommandList::ContinueIn(VkCommandBuffer commandBuffer) {
        m_commandBuffer = commandBuffer;
        m_layout = VK_NULL_HANDLE;
        m_bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        m_pushStages = 0;
    }

    void RhiCommandList::QueueMemoryBarrier(const VkMemoryBarrier2& barrier) {
        if (!m_memoryPending) {
            m_pendingMemory = barrier;
            m_memoryPending = true;
            return;
        }
        m_pendingMemory.srcStageMask |= barrier.srcStageMask;
        m_pendingMemory.srcAccessMask |= barrier.srcAccessMask;
        m_pendingMemory.dstStageMask |= barrier.dstStageMask;
        m_pendingMemory.dstAccessMask |= barrier.dstAccessMask;
    }

    void RhiCommandList::QueueImageBarrier(const VkImageMemoryBarrier2& barrier) {
        // Deux changements de layout des mêmes niveaux d'une image dans un appel n'ont pas d'ordre garanti : le premier
        // part avant. Plein : de même. Parcours de 16 au plus.
        const auto overlap = [](std::uint32_t firstA, std::uint32_t countA, std::uint32_t firstB, std::uint32_t countB) {
            // VK_REMAINING_MIP_LEVELS et VK_REMAINING_ARRAY_LAYERS : ~0u, jusqu'au bout.
            const std::uint64_t endA = countA == ~0u ? UINT64_MAX : std::uint64_t{firstA} + countA;
            const std::uint64_t endB = countB == ~0u ? UINT64_MAX : std::uint64_t{firstB} + countB;
            return firstA < endB && firstB < endA;
        };
        const VkImageSubresourceRange& range = barrier.subresourceRange;
        const auto first = m_pendingImages.begin();
        const bool again = std::any_of(first, first + m_pendingImageCount, [&](const VkImageMemoryBarrier2& pending) {
            const VkImageSubresourceRange& other = pending.subresourceRange;
            return pending.image == barrier.image && overlap(range.baseMipLevel, range.levelCount, other.baseMipLevel, other.levelCount)
                && overlap(range.baseArrayLayer, range.layerCount, other.baseArrayLayer, other.layerCount);
        });
        if (again || m_pendingImageCount == MaxPendingImages) {
            FlushBarriers();
        }
        m_pendingImages[m_pendingImageCount++] = barrier;
    }

    void RhiCommandList::QueueSwapchainTransition(VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                                                  VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = srcStage;
        barrier.srcAccessMask = srcAccess;
        barrier.dstStageMask = dstStage;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        QueueImageBarrier(barrier);
    }

    void RhiCommandList::FlushBarriers() {
        if (!m_memoryPending && m_pendingImageCount == 0) {
            return;
        }
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = m_memoryPending ? 1 : 0;
        dependency.pMemoryBarriers = &m_pendingMemory;
        dependency.imageMemoryBarrierCount = m_pendingImageCount;
        dependency.pImageMemoryBarriers = m_pendingImages.data();
        vkCmdPipelineBarrier2(m_commandBuffer, &dependency);
        m_memoryPending = false;
        m_pendingImageCount = 0;
    }

    std::span<RHI::CommandList* const> RhiCommandList::Fork(std::uint32_t count) {
        return m_canFork ? m_batch->Fork(*this, count) : std::span<RHI::CommandList* const>{};
    }

    void RhiCommandList::WriteTimestamp(std::uint32_t slot) {
        if (m_queryPool == VK_NULL_HANDLE || slot >= RHI::MaxTimestamps) {
            return;
        }
        vkCmdWriteTimestamp2(m_commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_queryPool, slot);
        m_writtenTimestamps |= 1u << slot;
    }

    void RhiCommandList::SetStepMarkers(const StepMarkers& markers) {
        m_markers = markers;
    }

    void RhiCommandList::BeginStep(const char* name) {
        // Aussi en repère nommé pour les outils de mesure (Radeon GPU Profiler...) quand VK_EXT_debug_utils est là.
        if (m_debugLabels) {
            if (m_labelOpen) {
                vkCmdEndDebugUtilsLabelEXT(m_commandBuffer);
            }
            VkDebugUtilsLabelEXT label{};
            label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
            label.pLabelName = name;
            vkCmdBeginDebugUtilsLabelEXT(m_commandBuffer, &label);
            m_labelOpen = true;
        }
        if (m_markers.buffer == VK_NULL_HANDLE) {
            return;
        }
        EndStep();
        // Case prise d'un coup : les listes parallèles notent leurs étapes en même temps.
        const std::uint32_t step = m_markers.count->fetch_add(1, std::memory_order_relaxed);
        if (step >= RHI::MaxSteps) {
            return;
        }
        m_openStep = step;
        (*m_markers.names)[m_openStep] = name;
        // Début : quand le GPU arrive à cette commande ; fin (EndStep) : quand tout ce qui précède est fini.
        vkCmdWriteBufferMarkerAMD(m_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_markers.buffer,
                                  m_markers.offset + m_openStep * 2 * sizeof(std::uint32_t), m_markers.serial);
    }

    void RhiCommandList::EndStep() {
        if (m_labelOpen) {
            vkCmdEndDebugUtilsLabelEXT(m_commandBuffer);
            m_labelOpen = false;
        }
        if (m_openStep >= RHI::MaxSteps) {
            return;
        }
        vkCmdWriteBufferMarkerAMD(m_commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_markers.buffer,
                                  m_markers.offset + (m_openStep * 2 + 1) * sizeof(std::uint32_t), m_markers.serial);
        m_openStep = RHI::MaxSteps;
    }

    namespace {
        struct AccessInfo
        {
            VkImageLayout layout;
            VkPipelineStageFlags2 stages;
            VkAccessFlags2 access;
            bool write;
        };

        // Étapes de tâche et de maillage, à attendre quand les mesh shaders sont activés (elles n'existent pas sinon).
        VkPipelineStageFlags2 MeshStages(bool meshShaders) {
            return meshShaders ? VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT : 0;
        }

        // Où et comment chaque usage touche une texture. Lue : par tous les shaders qui le peuvent (le sol lit des
        // tables par sommet, la pyramide la profondeur en calcul) ; plus précis ne gagnerait presque rien.
        AccessInfo Describe(RHI::TextureAccess access, bool meshShaders) {
            switch (access) {
                case RHI::TextureAccess::None: return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, false};
                case RHI::TextureAccess::ColorTarget:
                    return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true};
                case RHI::TextureAccess::DepthTarget:
                    return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, true};
                case RHI::TextureAccess::Sampled:
                    return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | MeshStages(meshShaders),
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false};
                case RHI::TextureAccess::Storage:
                    return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true};
            }
            return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, false};
        }
    }

    void RhiCommandList::UseTextures(std::span<const RHI::TextureUse> uses) {
        // Mises en attente avec les autres barrières : le pilote attend tout d'un coup au lieu de vider le GPU à chaque
        // texture ou à chaque appel.
        for (const RHI::TextureUse& use : uses) {
            const auto& texture = static_cast<const RhiTexture&>(*use.texture);
            // Usage prévu : l'usage noté n'est pas lu, un autre fil peut l'écrire (passes internes d'une autre liste).
            const RHI::TextureAccess from = use.from ? *use.from : texture.GetAccess();
            const AccessInfo before = Describe(from, m_meshShaders);
            const AccessInfo after = Describe(use.access, m_meshShaders);
            if (!use.from) {
                texture.SetAccess(use.access);
            }
            // Lue puis relue : rien à attendre. Écrite : toujours attendre ce qui précède, même déjà cible (la même
            // texture sert à chaque image, la précédente écrit peut-être encore).
            if (from == use.access && !after.write) {
                continue;
            }
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = before.stages;
            barrier.srcAccessMask = before.write ? before.access : VK_ACCESS_2_NONE; // après une lecture : l'ordre suffit
            barrier.dstStageMask = after.stages;
            barrier.dstAccessMask = after.access;
            barrier.oldLayout = use.discard ? VK_IMAGE_LAYOUT_UNDEFINED : before.layout;
            barrier.newLayout = after.layout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = texture.GetImage().GetHandle();
            barrier.subresourceRange = {texture.GetAspect(), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            QueueImageBarrier(barrier);
        }
    }

    void RhiCommandList::PlanTextures(std::span<RHI::TextureUse> uses) {
        for (RHI::TextureUse& use : uses) {
            const auto& texture = static_cast<const RhiTexture&>(*use.texture);
            use.from = texture.GetAccess();
            texture.SetAccess(use.access);
        }
    }

    // Tampon de profondeur d'une passe avec couleur, effacé ; son contenu n'est gardé (lisible après la passe) qu'avec
    // keepDepth.
    VkRenderingAttachmentInfo RhiCommandList::BeginDepthAttachment(const RHI::RenderPassDesc& desc) {
        const auto& depth = static_cast<const RhiTexture&>(*desc.depth);
        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = depth.GetImage().GetView();
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = desc.loadDepth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = desc.keepDepth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = {desc.clearDepth, 0};
        return depthAttachment;
    }

    void RhiCommandList::BeginRenderPass(const RHI::RenderPassDesc& desc) {
        if (!desc.color) {
            BeginDepthOnlyPass(desc, 0);
            return;
        }
        if (desc.colorTarget) {
            BeginTexturePass(desc, 0);
            return;
        }
        // Image de la swapchain : seule cible que la passe prépare elle-même (le graphe ne la voit pas). Le contenu
        // précédent est jeté : on efface tout de suite après.
        QueueSwapchainTransition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = m_view;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{desc.clearColor.r, desc.clearColor.g, desc.clearColor.b, desc.clearColor.a}};

        const VkRenderingAttachmentInfo depthAttachment = desc.depth ? BeginDepthAttachment(desc) : VkRenderingAttachmentInfo{};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, m_extent};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        renderingInfo.pDepthAttachment = desc.depth ? &depthAttachment : nullptr;
        FlushBarriers();
        vkCmdBeginRendering(m_commandBuffer, &renderingInfo);
    }

    // Texture 2D ou face d'un niveau de mip d'une texture cube : effacée puis remplie.
    void RhiCommandList::BeginTexturePass(const RHI::RenderPassDesc& desc, VkRenderingFlags flags) {
        const auto& target = static_cast<const RhiTexture&>(*desc.colorTarget);
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        const VkImageView faceView = target.GetImage().GetFaceView(desc.colorLayer, desc.colorMip);
        colorAttachment.imageView = faceView != VK_NULL_HANDLE ? faceView : target.GetImage().GetView();
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = desc.loadColor ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{desc.clearColor.r, desc.clearColor.g, desc.clearColor.b, desc.clearColor.a}};
        std::array<VkRenderingAttachmentInfo, 3> colorAttachments{colorAttachment, {}, {}};
        // Seconde et troisième cibles (la troisième seulement avec la seconde), effacées à 0 ; même couche et même
        // niveau que la première quand elles en ont (atlas des imposteurs).
        const std::array<const RHI::Texture*, 2> extras{desc.secondColorTarget, desc.secondColorTarget ? desc.thirdColorTarget : nullptr};
        std::uint32_t colorCount = 1;
        for (const RHI::Texture* extra : extras) {
            if (extra) {
                const auto& image = static_cast<const RhiTexture&>(*extra).GetImage();
                const VkImageView extraView = image.GetFaceView(desc.colorLayer, desc.colorMip);
                colorAttachments[colorCount] = colorAttachment;
                colorAttachments[colorCount].imageView = extraView != VK_NULL_HANDLE ? extraView : image.GetView();
                colorAttachments[colorCount].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
                ++colorCount;
            }
        }
        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        const VkRenderingAttachmentInfo depthAttachment = desc.depth ? BeginDepthAttachment(desc) : VkRenderingAttachmentInfo{};
        VkExtent2D area = target.GetImage().GetExtent();
        if (faceView != VK_NULL_HANDLE) {
            area = {std::max(area.width >> desc.colorMip, 1u), std::max(area.height >> desc.colorMip, 1u)};
        }
        renderingInfo.renderArea = {{0, 0}, area};
        renderingInfo.flags = flags;
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = colorCount;
        renderingInfo.pColorAttachments = colorAttachments.data();
        renderingInfo.pDepthAttachment = desc.depth ? &depthAttachment : nullptr;
        FlushBarriers();
        vkCmdBeginRendering(m_commandBuffer, &renderingInfo);
    }

    // Carte d'ombres : effacée, ou gardée (loadDepth) quand seules certaines cascades sont redessinées.
    void RhiCommandList::BeginDepthOnlyPass(const RHI::RenderPassDesc& desc, VkRenderingFlags flags) {
        const auto& depth = static_cast<const RhiTexture&>(*desc.depth);
        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = depth.GetImage().GetView();
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = desc.loadDepth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // lue ensuite
        depthAttachment.clearValue.depthStencil = {desc.clearDepth, 0};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, depth.GetImage().GetExtent()};
        renderingInfo.flags = flags;
        renderingInfo.layerCount = 1;
        renderingInfo.pDepthAttachment = &depthAttachment;
        FlushBarriers();
        vkCmdBeginRendering(m_commandBuffer, &renderingInfo);
    }

    std::span<RHI::CommandList* const> RhiCommandList::BeginParallelRenderPass(const RHI::RenderPassDesc& desc,
                                                                             std::uint32_t count) {
        // L'image de la swapchain reste dans la liste : sa passe est légère, et son format n'est pas noté ici.
        if (m_batch == nullptr || count == 0 || (desc.color && desc.colorTarget == nullptr)) {
            BeginRenderPass(desc);
            return {};
        }
        // Formats de la passe, que les listes secondaires doivent connaître d'avance.
        std::array<VkFormat, 3> colorFormats{};
        VkCommandBufferInheritanceRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
        if (desc.color) {
            colorFormats[0] = ToVulkan(desc.colorTarget->GetFormat());
            rendering.colorAttachmentCount = 1;
            if (desc.secondColorTarget) {
                colorFormats[1] = ToVulkan(desc.secondColorTarget->GetFormat());
                rendering.colorAttachmentCount = 2;
                if (desc.thirdColorTarget) {
                    colorFormats[2] = ToVulkan(desc.thirdColorTarget->GetFormat());
                    rendering.colorAttachmentCount = 3;
                }
            }
            rendering.pColorAttachmentFormats = colorFormats.data();
        }
        rendering.depthAttachmentFormat = desc.depth ? ToVulkan(desc.depth->GetFormat()) : VK_FORMAT_UNDEFINED;
        rendering.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        const std::span<RHI::CommandList* const> lists = m_batch->OpenSecondaries(*this, rendering, count);
        if (lists.empty()) {
            BeginRenderPass(desc);
            return {};
        }
        // Rien ne s'enregistre ici jusqu'à la fin de la passe : l'étape ouverte se ferme avant, chaque liste note les siennes.
        EndStep();
        if (desc.color) {
            BeginTexturePass(desc, VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);
        } else {
            BeginDepthOnlyPass(desc, VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);
        }
        m_secondaries = lists;
        return lists;
    }

    void RhiCommandList::EndRenderPass() {
        if (!m_secondaries.empty()) {
            std::array<VkCommandBuffer, RHI::MaxSecondaryCommandLists> buffers{};
            std::uint32_t count = 0;
            for (RHI::CommandList* list : m_secondaries) {
                auto& secondary = static_cast<RhiCommandList&>(*list);
                secondary.EndStep();
                if (Check(vkEndCommandBuffer(secondary.m_commandBuffer), "vkEndCommandBuffer")) {
                    buffers[count++] = secondary.m_commandBuffer;
                }
                m_writtenTimestamps |= secondary.m_writtenTimestamps;
            }
            if (count > 0) {
                vkCmdExecuteCommands(m_commandBuffer, count, buffers.data());
            }
            m_secondaries = {};
        }
        vkCmdEndRendering(m_commandBuffer);
    }

    void RhiCommandList::TransitionToPresent() {
        // Jusqu'au sémaphore que la présentation attend (signalé après ALL_COMMANDS) : avec NONE, rien n'obligeait le
        // changement de layout à finir avant lui (la validation de synchronisation le signalait).
        QueueSwapchainTransition(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE);
        FlushBarriers();
    }

    void RhiCommandList::CopyImageToReadback(VkBuffer target) {
        QueueSwapchainTransition(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        FlushBarriers();
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {m_extent.width, m_extent.height, 1};
        vkCmdCopyImageToBuffer(m_commandBuffer, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, 1, &region);
        // La copie, lisible par le CPU une fois l'image finie (fence), et retour au layout que TransitionToPresent
        // attend : envoyés avec sa propre barrière.
        VkMemoryBarrier2 toHost{};
        toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        QueueMemoryBarrier(toHost);
        QueueSwapchainTransition(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_NONE,
                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    }

    void RhiCommandList::SetViewport(const RHI::Viewport& viewport) {
        const VkViewport vkViewport{viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth};
        vkCmdSetViewport(m_commandBuffer, 0, 1, &vkViewport);
    }

    void RhiCommandList::SetScissor(const RHI::Rect& rect) {
        const VkRect2D scissor{{rect.x, rect.y}, {rect.width, rect.height}};
        vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);
    }

    void RhiCommandList::ClearDepth(const RHI::Rect& rect, float depth) {
        VkClearAttachment attachment{};
        attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        attachment.clearValue.depthStencil = {depth, 0};
        VkClearRect clearRect{};
        clearRect.rect = {{rect.x, rect.y}, {rect.width, rect.height}};
        clearRect.layerCount = 1;
        vkCmdClearAttachments(m_commandBuffer, 1, &attachment, 1, &clearRect);
    }

    void RhiCommandList::BindPipeline(const RHI::Pipeline& pipeline) {
        const Vk::Pipeline& vkPipeline = static_cast<const RhiPipeline&>(pipeline).Get();
        m_layout = vkPipeline.GetLayout();
        m_bindPoint = vkPipeline.GetBindPoint();
        m_pushStages = vkPipeline.GetPushStages();
        vkCmdBindPipeline(m_commandBuffer, m_bindPoint, vkPipeline.GetHandle());
    }

    void RhiCommandList::SetBindGroup(std::uint32_t index, const RHI::BindGroup& group) {
        const VkDescriptorSet set = static_cast<const RhiBindGroup&>(group).GetHandle();
        vkCmdBindDescriptorSets(m_commandBuffer, m_bindPoint, m_layout, index, 1, &set, 0, nullptr);
    }

    void RhiCommandList::BindVertexBuffers(std::uint32_t first, std::span<const RHI::Buffer* const> buffers) {
        std::array<VkBuffer, 4> handles{};
        constexpr std::array<VkDeviceSize, 4> offsets{};
        const auto count = static_cast<std::uint32_t>(std::min(buffers.size(), handles.size()));
        for (std::uint32_t i = 0; i < count; ++i) {
            handles[i] = static_cast<const RhiBuffer*>(buffers[i])->GetHandle();
        }
        vkCmdBindVertexBuffers(m_commandBuffer, first, count, handles.data(), offsets.data());
    }

    void RhiCommandList::BindIndexBuffer(const RHI::Buffer& buffer) {
        vkCmdBindIndexBuffer(m_commandBuffer, static_cast<const RhiBuffer&>(buffer).GetHandle(), 0, VK_INDEX_TYPE_UINT32);
    }

    void RhiCommandList::PushConstants(const void* data, std::uint32_t size) {
        vkCmdPushConstants(m_commandBuffer, m_layout, m_pushStages, 0, size, data);
    }

    void RhiCommandList::Draw(std::uint32_t vertexCount, std::uint32_t firstVertex) {
        vkCmdDraw(m_commandBuffer, vertexCount, 1, firstVertex, 0);
    }

    void RhiCommandList::DrawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex, std::uint32_t instance) {
        vkCmdDrawIndexed(m_commandBuffer, indexCount, 1, firstIndex, 0, instance);
    }

    void RhiCommandList::DrawIndexedIndirect(const RHI::Buffer& buffer, std::uint64_t offset, std::uint32_t count) {
        static_assert(sizeof(RHI::DrawIndexedCommand) == sizeof(VkDrawIndexedIndirectCommand));
        vkCmdDrawIndexedIndirect(m_commandBuffer, static_cast<const RhiBuffer&>(buffer).GetHandle(), offset, count,
                                 sizeof(RHI::DrawIndexedCommand));
    }

    void RhiCommandList::DrawMeshTasks(std::uint32_t groups) {
        vkCmdDrawMeshTasksEXT(m_commandBuffer, groups, 1, 1);
    }

    void RhiCommandList::Dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) {
        FlushBarriers();
        vkCmdDispatch(m_commandBuffer, groupsX, groupsY, groupsZ);
    }

    // Une barrière mémoire globale : plus simple qu'une par buffer, et les pilotes ne font guère mieux avec le détail.
    // Elle attend aussi les lectures des Dispatch précédents avant les écritures suivantes (buffer réutilisé). Celle
    // d'après reste en attente : elle part avec la barrière de la commande suivante.
    void RhiCommandList::CopyBuffer(const RHI::Buffer& source, std::uint64_t sourceOffset, RHI::Buffer& target,
                                    std::uint64_t targetOffset, std::uint64_t size) {
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        QueueMemoryBarrier(barrier);
        FlushBarriers();
        const VkBufferCopy region{sourceOffset, targetOffset, size};
        vkCmdCopyBuffer(m_commandBuffer, static_cast<const RhiBuffer&>(source).GetHandle(),
                        static_cast<RhiBuffer&>(target).GetHandle(), 1, &region);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_HOST_READ_BIT;
        QueueMemoryBarrier(barrier);
    }

    void RhiCommandList::BuildBottomLevels(std::span<RHI::AccelerationStructure* const> structures, const RHI::Buffer& scratch) {
        if (structures.empty()) {
            return;
        }
        // Sans barrière entre elles : chacune a sa place de travail, le GPU les construit en même temps.
        FlushBarriers();
        const VkBuffer scratchBuffer = static_cast<const RhiBuffer&>(scratch).GetHandle();
        VkDeviceSize offset = 0;
        for (RHI::AccelerationStructure* structure : structures) {
            auto& bottom = static_cast<RhiAccelerationStructure&>(*structure);
            bottom.RecordBuild(m_commandBuffer, scratchBuffer, offset);
            offset += bottom.GetScratchSize();
        }
        AccelerationBarrier();
    }

    void RhiCommandList::BuildTopLevel(RHI::AccelerationStructure& structure, std::span<const RHI::AccelerationInstance> instances,
                                       const RHI::Buffer& scratch) {
        FlushBarriers();
        static_cast<RhiAccelerationStructure&>(structure).RecordBuild(m_commandBuffer, static_cast<const RhiBuffer&>(scratch).GetHandle(),
                                                                      0, instances);
        AccelerationBarrier();
    }

    void RhiCommandList::AccelerationBarrier() {
        // Les rayons partent des shaders de dessin et de calcul (tâche et maillage compris).
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                             | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                             | MeshStages(m_meshShaders);
        barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        QueueMemoryBarrier(barrier);
    }

    void RhiCommandList::ComputeBarrier(RHI::ComputeReaders readers) {
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        // Les étapes de tâche et de maillage aussi : un calcul qui réécrit ce qu'elles lisaient (pyramide de
        // profondeur) attend leurs lectures.
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | MeshStages(m_meshShaders);
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        // Seulement les lecteurs demandés : chaque accès de destination coûte un vidage de cache (HOST_READ réécrit
        // tout le L2 en mémoire sur les GPU AMD, les accès indirects et sommets vident leurs caches dédiés).
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        if (RHI::HasFlag(readers, RHI::ComputeReaders::Draw)) {
            barrier.dstStageMask |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT
                                  | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                  | MeshStages(m_meshShaders);
            barrier.dstAccessMask |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT
                                   | VK_ACCESS_2_INDEX_READ_BIT;
        }
        if (RHI::HasFlag(readers, RHI::ComputeReaders::Host)) {
            barrier.dstStageMask |= VK_PIPELINE_STAGE_2_HOST_BIT;
            barrier.dstAccessMask |= VK_ACCESS_2_HOST_READ_BIT;
        }
        QueueMemoryBarrier(barrier);
    }

    void RhiCommandList::GenerateMipmaps(const RHI::Texture& texture) {
        const auto& rhiTexture = static_cast<const RhiTexture&>(texture);
        const Image& image = rhiTexture.GetImage();
        const std::uint32_t levels = image.GetMipLevels();
        const std::uint32_t layers = image.GetLayers();
        if (levels <= 1) {
            Use(texture, RHI::TextureAccess::Sampled);
            return;
        }
        const AccessInfo before = Describe(rhiTexture.GetAccess(), m_meshShaders);
        const AccessInfo sampled = Describe(RHI::TextureAccess::Sampled, m_meshShaders);
        std::array<VkImageMemoryBarrier2, 2> barriers{};
        const auto barrier = [&](std::uint32_t index, std::uint32_t level, std::uint32_t count, VkImageLayout from, VkImageLayout to,
                                 VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                                 VkAccessFlags2 dstAccess) {
            VkImageMemoryBarrier2& b = barriers[index];
            b = {};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = srcStage;
            b.srcAccessMask = srcAccess;
            b.dstStageMask = dstStage;
            b.dstAccessMask = dstAccess;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image.GetHandle();
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, layers};
        };
        // Mises en attente : envoyées avant chaque copie, la dernière avec la commande qui suit.
        const auto queue = [&](std::uint32_t count) {
            for (std::uint32_t i = 0; i < count; ++i) {
                QueueImageBarrier(barriers[i]);
            }
        };
        // Le niveau 0 est lu tel quel ; les autres, réécrits en entier, perdent leur contenu d'avant.
        barrier(0, 0, 1, before.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, before.stages,
                before.write ? before.access : VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(1, 1, levels - 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, before.stages, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        queue(2);
        const VkExtent2D extent = image.GetExtent();
        std::int32_t width = static_cast<std::int32_t>(extent.width);
        std::int32_t height = static_cast<std::int32_t>(extent.height);
        std::int32_t depth = static_cast<std::int32_t>(image.GetDepth());
        for (std::uint32_t level = 1; level < levels; ++level) {
            const std::int32_t nextWidth = std::max(width / 2, 1);
            const std::int32_t nextHeight = std::max(height / 2, 1);
            const std::int32_t nextDepth = std::max(depth / 2, 1);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, layers};
            blit.srcOffsets[1] = {width, height, depth};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers};
            blit.dstOffsets[1] = {nextWidth, nextHeight, nextDepth};
            FlushBarriers();
            vkCmdBlitImage(m_commandBuffer, image.GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.GetHandle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            // Ce niveau sera lu pour produire le suivant.
            barrier(0, level, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
            queue(1);
            width = nextWidth;
            height = nextHeight;
            depth = nextDepth;
        }
        barrier(0, 0, levels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sampled.layout, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT, sampled.stages, sampled.access);
        queue(1);
        rhiTexture.SetAccess(RHI::TextureAccess::Sampled);
    }
}
