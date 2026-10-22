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

    namespace {
        // Shaders qui lisent une texture remplie par une passe (BeginTexturePass) : par pixel, et par sommet (table de
        // l'air lue par les sommets du sol).
        constexpr VkPipelineStageFlags2 TexturePassReaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

        void TransitionImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout from, VkImageLayout to,
                             VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, std::uint32_t layer = 0) {
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
            barrier.subresourceRange = {aspect, 0, 1, layer, 1};

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

    // Tampon de profondeur d'une passe avec couleur, effacé ; son contenu n'est gardé (lisible après la passe) qu'avec
    // keepDepth.
    VkRenderingAttachmentInfo RhiCommandList::BeginDepthAttachment(const RHI::RenderPassDesc& desc) {
        const auto& depth = static_cast<const RhiTexture&>(*desc.depth);
        // Deux images en vol partagent le tampon de profondeur : on attend la fin des écritures de la précédente (et
        // de ses lectures s'il est gardé).
        TransitionImage(m_commandBuffer, depth.GetImage().GetHandle(),
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
                            | (desc.keepDepth ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                              : VK_PIPELINE_STAGE_2_NONE),
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        depth.GetAspect());
        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = depth.GetImage().GetView();
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = desc.keepDepth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = {desc.clearDepth, 0};
        if (desc.keepDepth) {
            m_keptDepth = &depth;
        }
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
        // Le contenu précédent est jeté : on efface tout de suite après.
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

    // Texture 2D ou face d'une texture cube : l'image précédente la lisait peut-être encore (même file : la barrière
    // attend ses lectures). Son contenu est jeté : la passe l'efface puis la remplit.
    void RhiCommandList::BeginTexturePass(const RHI::RenderPassDesc& desc) {
        const auto& target = static_cast<const RhiTexture&>(*desc.colorTarget);
        m_colorTarget = &target;
        m_colorLayer = desc.colorLayer;
        TransitionImage(m_commandBuffer, target.GetImage().GetHandle(), VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, TexturePassReaders, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, desc.colorLayer);
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        const VkImageView faceView = target.GetImage().GetFaceView(desc.colorLayer);
        colorAttachment.imageView = faceView != VK_NULL_HANDLE ? faceView : target.GetImage().GetView();
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{desc.clearColor.r, desc.clearColor.g, desc.clearColor.b, desc.clearColor.a}};
        std::array<VkRenderingAttachmentInfo, 2> colorAttachments{colorAttachment, {}};
        if (desc.secondColorTarget) {
            const auto& second = static_cast<const RhiTexture&>(*desc.secondColorTarget);
            m_secondTarget = &second;
            TransitionImage(m_commandBuffer, second.GetImage().GetHandle(), VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, TexturePassReaders, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, 0);
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

    // Carte d'ombres : l'image précédente la lisait peut-être encore (même file : la barrière attend ses lectures).
    void RhiCommandList::BeginDepthOnlyPass(const RHI::RenderPassDesc& desc) {
        const auto& depth = static_cast<const RhiTexture&>(*desc.depth);
        m_depthOnly = &depth;
        // Gardée : elle sort de la passe précédente lisible par les shaders (EndRenderPass), ses écritures à attendre
        // aussi ; sinon son contenu est jeté.
        TransitionImage(m_commandBuffer, depth.GetImage().GetHandle(),
                        desc.loadDepth ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        depth.GetAspect());
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
        if (const RhiTexture* depth = std::exchange(m_depthOnly, nullptr)) {
            TransitionImage(m_commandBuffer, depth->GetImage().GetHandle(),
                            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                            depth->GetAspect());
        }
        if (const RhiTexture* depth = std::exchange(m_keptDepth, nullptr)) {
            // Lue ensuite par l'anticrénelage (pixels) et la pyramide de profondeur (compute).
            TransitionImage(m_commandBuffer, depth->GetImage().GetHandle(),
                            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                            depth->GetAspect());
        }
        if (const RhiTexture* second = std::exchange(m_secondTarget, nullptr)) {
            TransitionImage(m_commandBuffer, second->GetImage().GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, TexturePassReaders,
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0);
        }
        if (const RhiTexture* target = std::exchange(m_colorTarget, nullptr)) {
            TransitionImage(m_commandBuffer, target->GetImage().GetHandle(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, TexturePassReaders,
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, m_colorLayer);
        }
    }

    void RhiCommandList::TransitionToPresent() {
        TransitionImage(m_commandBuffer, m_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
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

    void RhiCommandList::Dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) {
        vkCmdDispatch(m_commandBuffer, groupsX, groupsY, groupsZ);
    }

    // Une barrière mémoire globale : plus simple qu'une par buffer, et les pilotes ne font guère mieux avec le détail.
    // Elle attend aussi les lectures des Dispatch précédents avant les écritures suivantes (buffer réutilisé).
    void RhiCommandList::ComputeBarrier() {
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT
                             | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                             | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT
                              | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                              | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(m_commandBuffer, &dependency);
    }
}
