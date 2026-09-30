#include "Renderer/Vulkan/VulkanRHI.hpp"

#include <algorithm>

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

    namespace {
        void TransitionImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout from, VkImageLayout to,
                             VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
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
            barrier.subresourceRange = {aspect, 0, 1, 0, 1};

            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
        }
    }

    void RhiCommandList::Begin(VkCommandBuffer commandBuffer, VkImage image, VkImageView view, VkExtent2D extent) {
        m_commandBuffer = commandBuffer;
        m_image = image;
        m_view = view;
        m_extent = extent;
        m_layout = VK_NULL_HANDLE;
    }

    void RhiCommandList::BeginRenderPass(const RHI::RenderPassDesc& desc) {
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

        VkRenderingAttachmentInfo depthAttachment{};
        if (desc.depth) {
            const auto& depth = static_cast<const RhiTexture&>(*desc.depth);
            // Deux images en vol partagent le tampon de profondeur : on attend la fin des écritures de la précédente.
            TransitionImage(m_commandBuffer, depth.GetImage().GetHandle(),
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            depth.GetAspect());
            depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAttachment.imageView = depth.GetImage().GetView();
            depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthAttachment.clearValue.depthStencil = {desc.clearDepth, 0};
        }

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, m_extent};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        renderingInfo.pDepthAttachment = desc.depth ? &depthAttachment : nullptr;
        vkCmdBeginRendering(m_commandBuffer, &renderingInfo);
    }

    void RhiCommandList::EndRenderPass() {
        vkCmdEndRendering(m_commandBuffer);
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

    void RhiCommandList::BindPipeline(const RHI::Pipeline& pipeline) {
        const GraphicsPipeline& vkPipeline = static_cast<const RhiPipeline&>(pipeline).Get();
        m_layout = vkPipeline.GetLayout();
        vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipeline.GetHandle());
    }

    void RhiCommandList::SetBindGroup(std::uint32_t index, const RHI::BindGroup& group) {
        const VkDescriptorSet set = static_cast<const RhiBindGroup&>(group).GetHandle();
        vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, index, 1, &set, 0, nullptr);
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
        vkCmdPushConstants(m_commandBuffer, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
    }

    void RhiCommandList::Draw(std::uint32_t vertexCount, std::uint32_t firstVertex) {
        vkCmdDraw(m_commandBuffer, vertexCount, 1, firstVertex, 0);
    }

    void RhiCommandList::DrawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex) {
        vkCmdDrawIndexed(m_commandBuffer, indexCount, 1, firstIndex, 0, 0);
    }
}
