#include "Renderer/Vulkan/VulkanRHI.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace Flux::Vk {
    VkFormat ToVulkan(RHI::Format format) {
        switch (format) {
            case RHI::Format::Undefined: return VK_FORMAT_UNDEFINED;
            case RHI::Format::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
            case RHI::Format::RGBA8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
            case RHI::Format::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
            case RHI::Format::BGRA8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
            case RHI::Format::D32Float: return VK_FORMAT_D32_SFLOAT;
            case RHI::Format::RG32Float: return VK_FORMAT_R32G32_SFLOAT;
            case RHI::Format::RGB32Float: return VK_FORMAT_R32G32B32_SFLOAT;
            case RHI::Format::RGBA32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
            case RHI::Format::RGBA16Uint: return VK_FORMAT_R16G16B16A16_UINT;
            case RHI::Format::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case RHI::Format::RG16Float: return VK_FORMAT_R16G16_SFLOAT;
            case RHI::Format::R8Unorm: return VK_FORMAT_R8_UNORM;
            case RHI::Format::BC7Unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
            case RHI::Format::BC7Srgb: return VK_FORMAT_BC7_SRGB_BLOCK;
        }
        return VK_FORMAT_UNDEFINED;
    }

    RHI::Format FromVulkan(VkFormat format) {
        switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM: return RHI::Format::RGBA8Unorm;
            case VK_FORMAT_R8G8B8A8_SRGB: return RHI::Format::RGBA8Srgb;
            case VK_FORMAT_B8G8R8A8_UNORM: return RHI::Format::BGRA8Unorm;
            case VK_FORMAT_B8G8R8A8_SRGB: return RHI::Format::BGRA8Srgb;
            case VK_FORMAT_D32_SFLOAT: return RHI::Format::D32Float;
            default: return RHI::Format::Undefined;
        }
    }

    RhiBindGroupLayout::~RhiBindGroupLayout() {
        vkDestroyDescriptorPool(m_device, m_pool, nullptr); // ses groupes doivent déjà être rendus
        vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    }

    void RhiBindGroup::SetTexture(std::uint32_t slot, const RHI::Texture& texture, const RHI::Sampler& sampler) {
        const VkDescriptorImageInfo imageInfo{static_cast<const RhiSampler&>(sampler).GetHandle(),
                                              static_cast<const RhiTexture&>(texture).GetImage().GetView(),
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_set;
        write.dstBinding = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }

    void RhiBindGroup::SetBuffer(std::uint32_t slot, const RHI::Buffer& buffer) {
        const VkDescriptorBufferInfo bufferInfo{static_cast<const RhiBuffer&>(buffer).GetHandle(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_set;
        write.dstBinding = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }

    void RhiBindGroup::SetTableTexture(std::uint32_t slot, std::uint32_t element, const RHI::Texture& texture,
                                       const RHI::Sampler& sampler) {
        const VkDescriptorImageInfo imageInfo{static_cast<const RhiSampler&>(sampler).GetHandle(),
                                              static_cast<const RhiTexture&>(texture).GetImage().GetView(),
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_set;
        write.dstBinding = slot;
        write.dstArrayElement = element;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }

    void RhiBindGroup::SetStorageTexture(std::uint32_t slot, const RHI::Texture& texture) {
        const VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, static_cast<const RhiTexture&>(texture).GetImage().GetStorageView(),
                                              VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_set;
        write.dstBinding = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }

    namespace {
        // Image de la swapchain seulement : les textures passent par UseTextures.
        void TransitionImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout from, VkImageLayout to,
                             VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
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
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
        }
    }

    void RhiCommandList::Begin(VkCommandBuffer commandBuffer, VkImage image, VkImageView view, VkExtent2D extent,
                               VkQueryPool queryPool) {
        m_commandBuffer = commandBuffer;
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
        if (m_queryPool != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(m_commandBuffer, m_queryPool, 0, RHI::MaxTimestamps);
        }
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
        *m_markers.count = 0;
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
        if (*m_markers.count >= RHI::MaxSteps) {
            return;
        }
        m_openStep = (*m_markers.count)++;
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
        // Une barrière par lot : le pilote peut tout attendre d'un coup au lieu de vider le GPU à chaque texture.
        constexpr std::size_t MaxBatch = 16;
        std::array<VkImageMemoryBarrier2, MaxBatch> barriers{};
        std::uint32_t count = 0;
        const auto flush = [&] {
            if (count == 0) {
                return;
            }
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = count;
            dependency.pImageMemoryBarriers = barriers.data();
            vkCmdPipelineBarrier2(m_commandBuffer, &dependency);
            count = 0;
        };
        for (const RHI::TextureUse& use : uses) {
            const auto& texture = static_cast<const RhiTexture&>(*use.texture);
            const AccessInfo before = Describe(texture.GetAccess(), m_meshShaders);
            const AccessInfo after = Describe(use.access, m_meshShaders);
            // Lue puis relue : rien à attendre. Écrite : toujours attendre ce qui précède, même déjà cible (la même
            // texture sert à chaque image, la précédente écrit peut-être encore).
            if (texture.GetAccess() == use.access && !after.write) {
                continue;
            }
            if (count == MaxBatch) {
                flush();
            }
            VkImageMemoryBarrier2& barrier = barriers[count++];
            barrier = {};
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
            texture.SetAccess(use.access);
        }
        flush();
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
            BeginDepthOnlyPass(desc);
            return;
        }
        if (desc.colorTarget) {
            BeginTexturePass(desc);
            return;
        }
        // Image de la swapchain : seule cible que la passe prépare elle-même (le graphe ne la voit pas). Le contenu
        // précédent est jeté : on efface tout de suite après.
        TransitionImage(m_commandBuffer, m_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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
        vkCmdBeginRendering(m_commandBuffer, &renderingInfo);
    }

    // Texture 2D ou face d'une texture cube : effacée puis remplie.
    void RhiCommandList::BeginTexturePass(const RHI::RenderPassDesc& desc) {
        const auto& target = static_cast<const RhiTexture&>(*desc.colorTarget);
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        const VkImageView faceView = target.GetImage().GetFaceView(desc.colorLayer);
        colorAttachment.imageView = faceView != VK_NULL_HANDLE ? faceView : target.GetImage().GetView();
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = desc.loadColor ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{desc.clearColor.r, desc.clearColor.g, desc.clearColor.b, desc.clearColor.a}};
        std::array<VkRenderingAttachmentInfo, 2> colorAttachments{colorAttachment, {}};
        if (desc.secondColorTarget) {
            const auto& second = static_cast<const RhiTexture&>(*desc.secondColorTarget);
            colorAttachments[1] = colorAttachment;
            colorAttachments[1].imageView = second.GetImage().GetView();
            colorAttachments[1].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        }
        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        const VkRenderingAttachmentInfo depthAttachment = desc.depth ? BeginDepthAttachment(desc) : VkRenderingAttachmentInfo{};
        renderingInfo.renderArea = {{0, 0}, target.GetImage().GetExtent()};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = desc.secondColorTarget ? 2 : 1;
        renderingInfo.pColorAttachments = colorAttachments.data();
        renderingInfo.pDepthAttachment = desc.depth ? &depthAttachment : nullptr;
        vkCmdBeginRendering(m_commandBuffer, &renderingInfo);
    }

    // Carte d'ombres : effacée, ou gardée (loadDepth) quand seules certaines cascades sont redessinées.
    void RhiCommandList::BeginDepthOnlyPass(const RHI::RenderPassDesc& desc) {
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
        renderingInfo.layerCount = 1;
        renderingInfo.pDepthAttachment = &depthAttachment;
        vkCmdBeginRendering(m_commandBuffer, &renderingInfo);
    }

    void RhiCommandList::EndRenderPass() {
        vkCmdEndRendering(m_commandBuffer);
    }

    void RhiCommandList::TransitionToPresent() {
        // Jusqu'au sémaphore que la présentation attend (signalé après ALL_COMMANDS) : avec NONE, rien n'obligeait le
        // changement de layout à finir avant lui (la validation de synchronisation le signalait).
        TransitionImage(m_commandBuffer, m_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE);
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
        vkCmdDispatch(m_commandBuffer, groupsX, groupsY, groupsZ);
    }

    // Une barrière mémoire globale : plus simple qu'une par buffer, et les pilotes ne font guère mieux avec le détail.
    // Elle attend aussi les lectures des Dispatch précédents avant les écritures suivantes (buffer réutilisé).
    void RhiCommandList::CopyBuffer(const RHI::Buffer& source, std::uint64_t sourceOffset, RHI::Buffer& target,
                                    std::uint64_t targetOffset, std::uint64_t size) {
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(m_commandBuffer, &dependency);
        const VkBufferCopy region{sourceOffset, targetOffset, size};
        vkCmdCopyBuffer(m_commandBuffer, static_cast<const RhiBuffer&>(source).GetHandle(),
                        static_cast<RhiBuffer&>(target).GetHandle(), 1, &region);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_HOST_READ_BIT;
        vkCmdPipelineBarrier2(m_commandBuffer, &dependency);
    }

    void RhiCommandList::ComputeBarrier() {
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        // Les étapes de tâche et de maillage aussi : un calcul qui réécrit ce qu'elles lisaient (pyramide de
        // profondeur) attend leurs lectures.
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | MeshStages(m_meshShaders);
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT
                             | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                             | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT | MeshStages(m_meshShaders);
        barrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT
                              | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                              | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(m_commandBuffer, &dependency);
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
        const auto flush = [&](std::uint32_t count) {
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = count;
            dependency.pImageMemoryBarriers = barriers.data();
            vkCmdPipelineBarrier2(m_commandBuffer, &dependency);
        };
        // Le niveau 0 est lu tel quel ; les autres, réécrits en entier, perdent leur contenu d'avant.
        barrier(0, 0, 1, before.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, before.stages,
                before.write ? before.access : VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(1, 1, levels - 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, before.stages, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        flush(2);
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
            vkCmdBlitImage(m_commandBuffer, image.GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.GetHandle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            // Ce niveau sera lu pour produire le suivant.
            barrier(0, level, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
            flush(1);
            width = nextWidth;
            height = nextHeight;
            depth = nextDepth;
        }
        barrier(0, 0, levels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sampled.layout, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT, sampled.stages, sampled.access);
        flush(1);
        rhiTexture.SetAccess(RHI::TextureAccess::Sampled);
    }
}
