#include "Renderer/Vulkan/VulkanRHI.hpp"

#include <unordered_map>
#include <vector>

namespace Flux::RHI {
    std::expected<std::unique_ptr<Device>, std::string> Device::Create(void* nativeWindowHandle, Extent extent, bool vsync) {
        auto device = Vk::RhiDevice::Create(nativeWindowHandle, extent, vsync);
        if (!device) {
            return std::unexpected(device.error());
        }
        return std::unique_ptr<Device>(std::move(*device));
    }
}

namespace Flux::Vk {
    namespace {
        VkBufferUsageFlags ToVulkan(RHI::BufferUsage usage) {
            VkBufferUsageFlags flags = 0;
            if (RHI::HasFlag(usage, RHI::BufferUsage::Vertex)) {
                flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            }
            if (RHI::HasFlag(usage, RHI::BufferUsage::Index)) {
                flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            }
            if (RHI::HasFlag(usage, RHI::BufferUsage::Storage)) {
                flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            }
            return flags;
        }

        VkShaderStageFlags ToVulkan(RHI::ShaderStage stages) {
            VkShaderStageFlags flags = 0;
            if (RHI::HasFlag(stages, RHI::ShaderStage::Vertex)) {
                flags |= VK_SHADER_STAGE_VERTEX_BIT;
            }
            if (RHI::HasFlag(stages, RHI::ShaderStage::Fragment)) {
                flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            return flags;
        }

        VkDescriptorType ToVulkan(RHI::BindingType type) {
            return type == RHI::BindingType::Texture ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }

        VkFilter ToVulkan(RHI::Filter filter) {
            return filter == RHI::Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        }

        VkCompareOp ToVulkan(RHI::CompareOp op) {
            switch (op) {
                case RHI::CompareOp::Greater: return VK_COMPARE_OP_GREATER;
                case RHI::CompareOp::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
                case RHI::CompareOp::Less: return VK_COMPARE_OP_LESS;
                case RHI::CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
            }
            return VK_COMPARE_OP_ALWAYS;
        }
    }

    std::expected<std::unique_ptr<RhiDevice>, std::string> RhiDevice::Create(void* nativeWindowHandle, RHI::Extent extent, bool vsync) {
        // Le constructeur est privé : pas de std::make_unique.
        std::unique_ptr<RhiDevice> rhi(new RhiDevice());
        auto device = Vk::Device::Create(nativeWindowHandle);
        if (!device) {
            return std::unexpected(device.error());
        }
        rhi->m_device = std::move(*device);
        rhi->m_extent = extent;
        rhi->m_vsync = vsync;
        rhi->m_swapchain = std::make_unique<Swapchain>(*rhi->m_device);
        if (!rhi->m_swapchain->Recreate({extent.width, extent.height}, vsync)) {
            return std::unexpected("création de la swapchain");
        }
        if (!rhi->CreateFrames()) {
            return std::unexpected("création des ressources de frame");
        }
        return rhi;
    }

    RhiDevice::~RhiDevice() {
        if (!m_device) {
            return;
        }
        m_device->WaitIdle();
        const VkDevice device = m_device->GetHandle();
        for (Frame& frame : m_frames) {
            vkDestroyFence(device, frame.inFlight, nullptr);
            vkDestroySemaphore(device, frame.imageAvailable, nullptr);
            vkDestroyCommandPool(device, frame.commandPool, nullptr);
        }
        m_swapchain.reset();
    }

    bool RhiDevice::CreateFrames() {
        const VkDevice device = m_device->GetHandle();
        for (Frame& frame : m_frames) {
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolInfo.queueFamilyIndex = m_device->GetGraphicsQueueFamily();
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

            // Signalée dès le départ pour que la première attente ne bloque pas.
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (!Check(vkCreateFence(device, &fenceInfo, nullptr, &frame.inFlight), "vkCreateFence")) {
                return false;
            }
        }
        return true;
    }

    std::expected<std::unique_ptr<RHI::Buffer>, std::string> RhiDevice::CreateBuffer(const RHI::BufferDesc& desc, const void* data) {
        const VkBufferUsageFlags usage = ToVulkan(desc.usage);
        if (desc.memory == RHI::MemoryType::Upload) {
            auto buffer = Vk::Buffer::Create(m_device->GetAllocator(), desc.size, usage, Vk::Buffer::Memory::Upload);
            if (!buffer) {
                return std::unexpected(buffer.error());
            }
            if (data) {
                buffer->Write(data, desc.size);
            }
            return std::make_unique<RhiBuffer>(std::move(*buffer));
        }

        // Mémoire vidéo, remplie par une copie depuis un buffer intermédiaire (attend la fin de la copie).
        auto buffer = Vk::Buffer::Create(m_device->GetAllocator(), desc.size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     Vk::Buffer::Memory::GpuOnly);
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        if (data) {
            auto staging = Vk::Buffer::Create(m_device->GetAllocator(), desc.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Vk::Buffer::Memory::Upload);
            if (!staging) {
                return std::unexpected(staging.error());
            }
            staging->Write(data, desc.size);
            const bool copied = m_device->ImmediateSubmit([&](VkCommandBuffer commandBuffer) {
                const VkBufferCopy region{0, 0, desc.size};
                vkCmdCopyBuffer(commandBuffer, staging->GetHandle(), buffer->GetHandle(), 1, &region);
            });
            if (!copied) {
                return std::unexpected("envoi d'un buffer au GPU");
            }
        }
        return std::make_unique<RhiBuffer>(std::move(*buffer));
    }

    std::expected<std::unique_ptr<RHI::Texture>, std::string> RhiDevice::CreateTexture(RHI::Format format, RHI::Extent extent,
                                                                                      std::span<const std::uint8_t> pixels,
                                                                                      bool mipmaps) {
        auto image = Image::CreateTexture(*m_device, ToVulkan(format), {extent.width, extent.height}, pixels, mipmaps);
        if (!image) {
            return std::unexpected(image.error());
        }
        return std::make_unique<RhiTexture>(std::move(*image), format, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    std::expected<std::unique_ptr<RHI::Texture>, std::string> RhiDevice::CreateRenderTarget(RHI::Format format, RHI::Extent extent) {
        const bool depth = format == RHI::Format::D32Float;
        const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        auto image = Image::CreateAttachment(*m_device, ToVulkan(format), {extent.width, extent.height},
                                             depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                             aspect);
        if (!image) {
            return std::unexpected(image.error());
        }
        return std::make_unique<RhiTexture>(std::move(*image), format, aspect);
    }

    std::expected<std::unique_ptr<RHI::Sampler>, std::string> RhiDevice::CreateSampler(const RHI::SamplerDesc& desc) {
        const VkSamplerAddressMode address =
            desc.address == RHI::AddressMode::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = ToVulkan(desc.filter);
        samplerInfo.minFilter = ToVulkan(desc.filter);
        samplerInfo.mipmapMode = desc.mipFilter == RHI::Filter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = address;
        samplerInfo.addressModeV = address;
        samplerInfo.addressModeW = address;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE; // tous les niveaux de mip de la texture
        VkSampler sampler = VK_NULL_HANDLE;
        if (!Check(vkCreateSampler(m_device->GetHandle(), &samplerInfo, nullptr, &sampler), "vkCreateSampler")) {
            return std::unexpected("création d'un sampler");
        }
        return std::make_unique<RhiSampler>(m_device->GetHandle(), sampler);
    }

    std::expected<std::unique_ptr<RHI::BindGroupLayout>, std::string> RhiDevice::CreateBindGroupLayout(const RHI::BindGroupLayoutDesc& desc) {
        const VkDevice device = m_device->GetHandle();
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::unordered_map<VkDescriptorType, std::uint32_t> counts;
        for (const RHI::Binding& binding : desc.bindings) {
            const VkDescriptorType type = ToVulkan(binding.type);
            bindings.push_back({binding.slot, type, 1, ToVulkan(binding.stages), nullptr});
            counts[type] += desc.maxGroups;
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        if (!Check(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout), "vkCreateDescriptorSetLayout")) {
            return std::unexpected("création d'un descriptor set layout");
        }

        // Groupes libérables un par un : un modèle rechargé rend ceux de l'ancienne version.
        std::vector<VkDescriptorPoolSize> sizes;
        for (const auto& [type, count] : counts) {
            sizes.push_back({type, count});
        }
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = desc.maxGroups;
        poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (!Check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool")) {
            vkDestroyDescriptorSetLayout(device, layout, nullptr);
            return std::unexpected("création d'un descriptor pool");
        }
        return std::make_unique<RhiBindGroupLayout>(device, layout, pool);
    }

    std::unique_ptr<RHI::BindGroup> RhiDevice::CreateBindGroup(const RHI::BindGroupLayout& layout) {
        const auto& vkLayout = static_cast<const RhiBindGroupLayout&>(layout);
        const VkDescriptorSetLayout setLayout = vkLayout.GetHandle();
        VkDescriptorSetAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateInfo.descriptorPool = vkLayout.GetPool();
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts = &setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        // Pool épuisé : pas une erreur fatale, l'appelant choisit quoi faire.
        if (vkAllocateDescriptorSets(m_device->GetHandle(), &allocateInfo, &set) != VK_SUCCESS) {
            return nullptr;
        }
        return std::make_unique<RhiBindGroup>(m_device->GetHandle(), vkLayout.GetPool(), set);
    }

    std::expected<std::unique_ptr<RHI::Pipeline>, std::string> RhiDevice::CreatePipeline(const RHI::PipelineDesc& desc) {
        std::vector<VkVertexInputBindingDescription> bindings;
        for (const RHI::VertexBufferLayout& layout : desc.vertexBuffers) {
            bindings.push_back({layout.binding, layout.stride, VK_VERTEX_INPUT_RATE_VERTEX});
        }
        std::vector<VkVertexInputAttributeDescription> attributes;
        for (const RHI::VertexAttribute& attribute : desc.vertexAttributes) {
            attributes.push_back({attribute.location, attribute.binding, Vk::ToVulkan(attribute.format), attribute.offset});
        }
        std::vector<VkDescriptorSetLayout> setLayouts;
        for (const RHI::BindGroupLayout* layout : desc.bindGroupLayouts) {
            setLayouts.push_back(static_cast<const RhiBindGroupLayout*>(layout)->GetHandle());
        }
        const VkPushConstantRange pushConstants{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, desc.pushConstantsSize};

        GraphicsPipelineDesc vkDesc;
        vkDesc.shaderPath = desc.shaderPath;
        vkDesc.vertexEntryPoint = desc.vertexEntryPoint;
        vkDesc.fragmentEntryPoint = desc.fragmentEntryPoint;
        vkDesc.vertexBindings = bindings;
        vkDesc.vertexAttributes = attributes;
        vkDesc.setLayouts = setLayouts;
        if (desc.pushConstantsSize > 0) {
            vkDesc.pushConstants = {&pushConstants, 1};
        }
        vkDesc.colorFormat = Vk::ToVulkan(desc.colorFormat);
        vkDesc.depthFormat = Vk::ToVulkan(desc.depthFormat);
        vkDesc.depthTest = desc.depthTest;
        vkDesc.depthWrite = desc.depthWrite;
        vkDesc.depthCompare = ToVulkan(desc.depthCompare);
        vkDesc.cullMode = desc.cullMode == RHI::CullMode::Back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        vkDesc.alphaBlend = desc.blend == RHI::BlendMode::Alpha;
        auto pipeline = GraphicsPipeline::Create(m_device->GetHandle(), vkDesc);
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        return std::make_unique<RhiPipeline>(std::move(*pipeline));
    }

    bool RhiDevice::ResizeSwapchain(RHI::Extent extent, bool vsync) {
        const VkExtent2D current = m_swapchain->GetExtent();
        if (extent.width == current.width && extent.height == current.height && vsync == m_vsync) {
            return true;
        }
        m_extent = extent;
        m_vsync = vsync;
        return RecreateSwapchain();
    }

    bool RhiDevice::RecreateSwapchain() {
        m_device->WaitIdle();
        return m_swapchain->Recreate({m_extent.width, m_extent.height}, m_vsync);
    }

    RHI::Extent RhiDevice::GetSwapchainExtent() const {
        const VkExtent2D extent = m_swapchain->GetExtent();
        return {extent.width, extent.height};
    }

    RHI::Format RhiDevice::GetSwapchainFormat() const {
        return FromVulkan(m_swapchain->GetFormat());
    }

    RHI::PresentMode RhiDevice::GetPresentMode() const {
        switch (m_swapchain->GetPresentMode()) {
            case VK_PRESENT_MODE_FIFO_KHR: return RHI::PresentMode::Fifo;
            case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return RHI::PresentMode::FifoRelaxed;
            case VK_PRESENT_MODE_MAILBOX_KHR: return RHI::PresentMode::Mailbox;
            case VK_PRESENT_MODE_IMMEDIATE_KHR: return RHI::PresentMode::Immediate;
            default: return RHI::PresentMode::Other;
        }
    }

    RHI::FrameStatus RhiDevice::BeginFrame() {
        Frame& frame = m_frames[m_frameIndex];
        const VkDevice device = m_device->GetHandle();
        if (!Check(vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences")) {
            return RHI::FrameStatus::Error;
        }

        const VkResult acquire = vkAcquireNextImageKHR(device, m_swapchain->GetHandle(), UINT64_MAX, frame.imageAvailable,
                                                       VK_NULL_HANDLE, &m_imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            return RecreateSwapchain() ? RHI::FrameStatus::Skip : RHI::FrameStatus::Error; // on retentera à l'image suivante
        }
        if (!Check(acquire, "vkAcquireNextImageKHR")) {
            return RHI::FrameStatus::Error;
        }

        // Réinitialisée seulement une fois sûr de soumettre, sinon l'image suivante attendrait indéfiniment.
        vkResetFences(device, 1, &frame.inFlight);
        vkResetCommandPool(device, frame.commandPool, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
        m_commandList.Begin(frame.commandBuffer, m_swapchain->GetImage(m_imageIndex), m_swapchain->GetImageView(m_imageIndex),
                            m_swapchain->GetExtent());
        return RHI::FrameStatus::Ready;
    }

    bool RhiDevice::EndFrame() {
        Frame& frame = m_frames[m_frameIndex];
        m_commandList.TransitionToPresent();
        if (!Check(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer")) {
            return false;
        }

        const VkSemaphore renderFinished = m_swapchain->GetRenderFinishedSemaphore(m_imageIndex);

        VkSemaphoreSubmitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waitInfo.semaphore = frame.imageAvailable;
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signalInfo{};
        signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalInfo.semaphore = renderFinished;
        signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo commandInfo{};
        commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        commandInfo.commandBuffer = frame.commandBuffer;

        VkSubmitInfo2 submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submitInfo.waitSemaphoreInfoCount = 1;
        submitInfo.pWaitSemaphoreInfos = &waitInfo;
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &commandInfo;
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos = &signalInfo;
        if (!Check(vkQueueSubmit2(m_device->GetGraphicsQueue(), 1, &submitInfo, frame.inFlight), "vkQueueSubmit2")) {
            return false;
        }

        const VkSwapchainKHR swapchain = m_swapchain->GetHandle();
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &m_imageIndex;
        const VkResult present = vkQueuePresentKHR(m_device->GetPresentQueue(), &presentInfo);

        m_frameIndex = (m_frameIndex + 1) % RHI::FramesInFlight;

        if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR) {
            return RecreateSwapchain();
        }
        return Check(present, "vkQueuePresentKHR");
    }
}
