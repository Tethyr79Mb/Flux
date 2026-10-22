#include "Renderer/Vulkan/VulkanRHI.hpp"

#include "Flux/Core/Version.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
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

    std::expected<std::unique_ptr<Device>, std::string> Device::CreateHeadless() {
        return Create(nullptr, {}, false);
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
            if (RHI::HasFlag(usage, RHI::BufferUsage::Indirect)) {
                flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
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
            if (RHI::HasFlag(stages, RHI::ShaderStage::Compute)) {
                flags |= VK_SHADER_STAGE_COMPUTE_BIT;
            }
            return flags;
        }

        VkDescriptorType ToVulkan(RHI::BindingType type) {
            return type == RHI::BindingType::StorageBuffer ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }

        VkFilter ToVulkan(RHI::Filter filter) {
            return filter == RHI::Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        }

        VkCompareOp ToVulkan(RHI::CompareOp op) {
            switch (op) {
                case RHI::CompareOp::Greater: return VK_COMPARE_OP_GREATER;
                case RHI::CompareOp::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
                case RHI::CompareOp::Less: return VK_COMPARE_OP_LESS;
                case RHI::CompareOp::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
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
        if (nativeWindowHandle) {
            rhi->m_swapchain = std::make_unique<Swapchain>(*rhi->m_device);
            if (!rhi->m_swapchain->Recreate({extent.width, extent.height}, vsync)) {
                return std::unexpected("création de la swapchain");
            }
            // Avec des images : les envois partent avec la suivante, sans attendre le GPU.
            rhi->m_device->SetDeferredUploads(true);
        }
        rhi->m_timestampPeriod = rhi->m_device->GetTimestampPeriod();
        rhi->m_commandList.SetDebugLabels(rhi->m_device->HasDebugLabels());
        // Repères du diagnostic de plantage : facultatifs, l'image se dessine sans.
        if (nativeWindowHandle && rhi->m_device->HasBufferMarkers()) {
            auto markers = Vk::Buffer::Create(rhi->m_device->GetAllocator(),
                                              RHI::FramesInFlight * RHI::MaxSteps * 2 * sizeof(std::uint32_t),
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT, Vk::Buffer::Memory::Readback);
            if (markers) {
                rhi->m_stepMarkers = std::move(*markers);
            }
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
            m_device->ReleaseUploads(frame.uploads);
            vkDestroyQueryPool(device, frame.queries, nullptr);
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

            // Heures du GPU (mesure des passes) : facultatives, l'image se dessine sans.
            if (m_timestampPeriod > 0.0) {
                VkQueryPoolCreateInfo queryInfo{};
                queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
                queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
                queryInfo.queryCount = RHI::MaxTimestamps;
                if (vkCreateQueryPool(device, &queryInfo, nullptr, &frame.queries) != VK_SUCCESS) {
                    frame.queries = VK_NULL_HANDLE;
                }
            }
        }
        return true;
    }

    void RhiDevice::ReadTimestamps(const Frame& frame) {
        m_timestampCount = 0;
        if (frame.queries == VK_NULL_HANDLE || frame.writtenTimestamps == 0) {
            return;
        }
        // Valeur et disponibilité par case ; le GPU a fini l'image (barrière attendue) : pas d'attente ici.
        std::array<std::uint64_t, 2 * RHI::MaxTimestamps> values{};
        const VkResult result = vkGetQueryPoolResults(m_device->GetHandle(), frame.queries, 0, RHI::MaxTimestamps,
                                                      sizeof(values), values.data(), 2 * sizeof(std::uint64_t),
                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result != VK_SUCCESS && result != VK_NOT_READY) {
            return;
        }
        const bool firstReady = (frame.writtenTimestamps & 1u) != 0 && values[1] != 0;
        for (std::uint32_t slot = 0; slot < RHI::MaxTimestamps; ++slot) {
            const bool ready = firstReady && (frame.writtenTimestamps & (1u << slot)) != 0 && values[2 * slot + 1] != 0;
            m_timestamps[slot] = ready ? static_cast<double>(values[2 * slot] - values[0]) * m_timestampPeriod * 1.0e-6 : -1.0;
            if (ready) {
                m_timestampCount = slot + 1;
            }
        }
    }

    std::expected<std::unique_ptr<RHI::Buffer>, std::string> RhiDevice::CreateBuffer(const RHI::BufferDesc& desc, const void* data) {
        const VkBufferUsageFlags usage = ToVulkan(desc.usage);
        if (desc.memory == RHI::MemoryType::Readback) {
            auto buffer = Vk::Buffer::Create(m_device->GetAllocator(), desc.size, usage, Vk::Buffer::Memory::Readback);
            if (!buffer) {
                return std::unexpected(buffer.error());
            }
            return std::make_unique<RhiBuffer>(std::move(*buffer));
        }
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

        // Mémoire vidéo, remplie par une copie depuis un buffer intermédiaire, soumise avec la prochaine image.
        auto buffer = Vk::Buffer::Create(m_device->GetAllocator(), desc.size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     Vk::Buffer::Memory::GpuOnly);
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        if (data) {
            auto staging = Vk::Buffer::Create(m_device->GetAllocator(), desc.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Vk::Buffer::Memory::Staging);
            if (!staging) {
                return std::unexpected(staging.error());
            }
            staging->Write(data, desc.size);
            const VkBuffer source = staging->GetHandle();
            const bool copied = m_device->Upload([&](VkCommandBuffer commandBuffer) {
                const VkBufferCopy region{0, 0, desc.size};
                vkCmdCopyBuffer(commandBuffer, source, buffer->GetHandle(), 1, &region);
            }, std::move(*staging));
            if (!copied) {
                return std::unexpected("envoi d'un buffer au GPU");
            }
        }
        return std::make_unique<RhiBuffer>(std::move(*buffer));
    }

    bool RhiDevice::WriteBuffer(RHI::Buffer& buffer, std::uint64_t offset, const void* data, std::uint64_t size) {
        auto& target = static_cast<RhiBuffer&>(buffer);
        if (size == 0) {
            return true;
        }
        if (target.IsMapped()) {
            target.Write(data, size, offset);
            return true;
        }
        auto staging = Vk::Buffer::Create(m_device->GetAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Vk::Buffer::Memory::Staging);
        if (!staging) {
            return false;
        }
        staging->Write(data, size);
        const VkBuffer source = staging->GetHandle();
        return m_device->Upload([&](VkCommandBuffer commandBuffer) {
            const VkBufferCopy region{0, offset, size};
            vkCmdCopyBuffer(commandBuffer, source, target.GetHandle(), 1, &region);
        }, std::move(*staging));
    }

    std::expected<std::unique_ptr<RHI::Texture>, std::string> RhiDevice::CreateTexture(RHI::Format format, RHI::Extent extent,
                                                                                      std::span<const std::uint8_t> pixels,
                                                                                      bool mipmaps) {
        auto image = Image::CreateTexture(*m_device, ToVulkan(format), {extent.width, extent.height}, pixels, mipmaps);
        if (!image) {
            return std::unexpected(image.error());
        }
        return std::make_unique<RhiTexture>(std::move(*image), format, VK_IMAGE_ASPECT_COLOR_BIT, RHI::TextureAccess::Sampled);
    }

    std::expected<std::unique_ptr<RHI::Texture>, std::string> RhiDevice::CreateCubeTexture(RHI::Format format, std::uint32_t size,
                                                                                          std::span<const std::uint8_t> pixels,
                                                                                          bool mipmaps) {
        auto image = Image::CreateTexture(*m_device, ToVulkan(format), {size, size}, pixels, mipmaps, ImageLayers::Cube);
        if (!image) {
            return std::unexpected(image.error());
        }
        return std::make_unique<RhiTexture>(std::move(*image), format, VK_IMAGE_ASPECT_COLOR_BIT, RHI::TextureAccess::Sampled);
    }

    std::expected<std::unique_ptr<RHI::Texture>, std::string> RhiDevice::CreateTextureArray(RHI::Format format, RHI::Extent extent,
                                                                                           std::uint32_t layers,
                                                                                           std::span<const std::uint8_t> pixels,
                                                                                           bool mipmaps) {
        auto image = Image::CreateTexture(*m_device, ToVulkan(format), {extent.width, extent.height}, pixels, mipmaps,
                                          ImageLayers::Array, layers);
        if (!image) {
            return std::unexpected(image.error());
        }
        return std::make_unique<RhiTexture>(std::move(*image), format, VK_IMAGE_ASPECT_COLOR_BIT, RHI::TextureAccess::Sampled);
    }

    std::expected<std::unique_ptr<RHI::Texture>, std::string> RhiDevice::CreateTextureArrayLevels(RHI::Format format, RHI::Extent extent,
                                                                                                 std::uint32_t layers, std::uint32_t levels,
                                                                                                 std::span<const std::uint8_t> pixels) {
        auto image = Image::CreateTextureLevels(*m_device, ToVulkan(format), {extent.width, extent.height}, pixels, levels,
                                                ImageLayers::Array, layers);
        if (!image) {
            return std::unexpected(image.error());
        }
        return std::make_unique<RhiTexture>(std::move(*image), format, VK_IMAGE_ASPECT_COLOR_BIT, RHI::TextureAccess::Sampled);
    }

    std::expected<std::unique_ptr<RHI::Texture>, std::string> RhiDevice::CreateRenderTarget(RHI::Format format, RHI::Extent extent,
                                                                                            bool sampled) {
        const bool depth = format == RHI::Format::D32Float;
        const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageUsageFlags usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (sampled) {
            usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        }
        auto image = Image::CreateAttachment(*m_device, ToVulkan(format), {extent.width, extent.height}, usage, aspect);
        if (!image) {
            return std::unexpected(image.error());
        }
        return std::make_unique<RhiTexture>(std::move(*image), format, aspect, RHI::TextureAccess::None);
    }

    std::expected<std::unique_ptr<RHI::Texture>, std::string> RhiDevice::CreateCubeRenderTarget(RHI::Format format,
                                                                                                std::uint32_t size) {
        auto image = Image::CreateCubeAttachment(*m_device, ToVulkan(format), size,
                                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (!image) {
            return std::unexpected(image.error());
        }
        return std::make_unique<RhiTexture>(std::move(*image), format, VK_IMAGE_ASPECT_COLOR_BIT, RHI::TextureAccess::None);
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
        if (desc.anisotropy > 1.0f && m_device->HasAnisotropy()) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(m_device->GetBootstrapDevice().physical_device.physical_device, &properties);
            samplerInfo.anisotropyEnable = VK_TRUE;
            samplerInfo.maxAnisotropy = std::min(desc.anisotropy, properties.limits.maxSamplerAnisotropy);
        }
        if (desc.compare) {
            samplerInfo.compareEnable = VK_TRUE;
            samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL; // éclairé si le point est devant l'obstacle le plus proche
        }
        VkSampler sampler = VK_NULL_HANDLE;
        if (!Check(vkCreateSampler(m_device->GetHandle(), &samplerInfo, nullptr, &sampler), "vkCreateSampler")) {
            return std::unexpected("création d'un sampler");
        }
        return std::make_unique<RhiSampler>(m_device->GetHandle(), sampler);
    }

    std::expected<std::unique_ptr<RHI::BindGroupLayout>, std::string> RhiDevice::CreateBindGroupLayout(const RHI::BindGroupLayoutDesc& desc) {
        const VkDevice device = m_device->GetHandle();
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        // Tables de textures : cases non remplies permises, remplies pendant que le GPU dessine avec d'autres
        // (« descriptor indexing », Vulkan 1.2, demandé à la création du device).
        std::vector<VkDescriptorBindingFlags> flags;
        bool table = false;
        std::unordered_map<VkDescriptorType, std::uint32_t> counts;
        for (const RHI::Binding& binding : desc.bindings) {
            const VkDescriptorType type = ToVulkan(binding.type);
            const bool isTable = binding.type == RHI::BindingType::TextureTable;
            const std::uint32_t count = isTable ? binding.count : 1;
            bindings.push_back({binding.slot, type, count, ToVulkan(binding.stages), nullptr});
            flags.push_back(isTable ? VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
                                          | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT
                                    : 0);
            table = table || isTable;
            counts[type] += count * desc.maxGroups;
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
        flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagsInfo.bindingCount = static_cast<std::uint32_t>(flags.size());
        flagsInfo.pBindingFlags = flags.data();
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.pNext = table ? &flagsInfo : nullptr;
        layoutInfo.flags = table ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
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
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT
                       | (table ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0);
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
        vkDesc.secondColorFormat = Vk::ToVulkan(desc.secondColorFormat);
        vkDesc.writeSecondColor = desc.writeSecondColor;
        vkDesc.writeColor = desc.writeColor;
        vkDesc.depthFormat = Vk::ToVulkan(desc.depthFormat);
        vkDesc.depthTest = desc.depthTest;
        vkDesc.depthWrite = desc.depthWrite;
        vkDesc.depthCompare = ToVulkan(desc.depthCompare);
        switch (desc.cullMode) {
            case RHI::CullMode::None: vkDesc.cullMode = VK_CULL_MODE_NONE; break;
            case RHI::CullMode::Back: vkDesc.cullMode = VK_CULL_MODE_BACK_BIT; break;
            case RHI::CullMode::Front: vkDesc.cullMode = VK_CULL_MODE_FRONT_BIT; break;
        }
        vkDesc.alphaBlend = desc.blend == RHI::BlendMode::Alpha;
        vkDesc.additiveBlend = desc.blend == RHI::BlendMode::Additive;
        vkDesc.depthBiasConstant = desc.depthBiasConstant;
        vkDesc.depthBiasSlope = desc.depthBiasSlope;
        auto pipeline = Pipeline::CreateGraphics(m_device->GetHandle(), vkDesc);
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        return std::make_unique<RhiPipeline>(std::move(*pipeline));
    }

    std::expected<std::unique_ptr<RHI::Pipeline>, std::string> RhiDevice::CreateComputePipeline(const RHI::ComputePipelineDesc& desc) {
        std::vector<VkDescriptorSetLayout> setLayouts;
        for (const RHI::BindGroupLayout* layout : desc.bindGroupLayouts) {
            setLayouts.push_back(static_cast<const RhiBindGroupLayout*>(layout)->GetHandle());
        }
        ComputePipelineDesc vkDesc;
        vkDesc.shaderPath = desc.shaderPath;
        vkDesc.entryPoint = desc.entryPoint;
        vkDesc.setLayouts = setLayouts;
        vkDesc.pushConstantsSize = desc.pushConstantsSize;
        auto pipeline = Pipeline::CreateCompute(m_device->GetHandle(), vkDesc);
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        return std::make_unique<RhiPipeline>(std::move(*pipeline));
    }

    bool RhiDevice::Submit(const std::function<void(RHI::CommandList&)>& record) {
        return m_device->ImmediateSubmit([&](VkCommandBuffer commandBuffer) {
            RhiCommandList commandList;
            commandList.Begin(commandBuffer, VK_NULL_HANDLE, VK_NULL_HANDLE, {}, VK_NULL_HANDLE);
            record(commandList);
        });
    }

    bool RhiDevice::ResizeSwapchain(RHI::Extent extent, bool vsync) {
        if (!m_swapchain) {
            return false;
        }
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
        if (!m_swapchain) {
            return {};
        }
        const VkExtent2D extent = m_swapchain->GetExtent();
        return {extent.width, extent.height};
    }

    RHI::Format RhiDevice::GetSwapchainFormat() const {
        return m_swapchain ? FromVulkan(m_swapchain->GetFormat()) : RHI::Format::Undefined;
    }

    // Compteurs tenus par VMA (pas de parcours des blocs) : assez léger pour chaque image.
    RHI::MemoryUsage RhiDevice::GetMemoryUsage() const {
        const VkPhysicalDeviceMemoryProperties* properties = nullptr;
        vmaGetMemoryProperties(m_device->GetAllocator(), &properties);
        std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
        vmaGetHeapBudgets(m_device->GetAllocator(), budgets.data());
        RHI::MemoryUsage usage;
        for (std::uint32_t heap = 0; heap < properties->memoryHeapCount; ++heap) {
            const bool device = (properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
            (device ? usage.device : usage.host) += budgets[heap].statistics.blockBytes;
        }
        return usage;
    }

    RHI::PresentMode RhiDevice::GetPresentMode() const {
        if (!m_swapchain) {
            return RHI::PresentMode::Other;
        }
        switch (m_swapchain->GetPresentMode()) {
            case VK_PRESENT_MODE_FIFO_KHR: return RHI::PresentMode::Fifo;
            case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return RHI::PresentMode::FifoRelaxed;
            case VK_PRESENT_MODE_MAILBOX_KHR: return RHI::PresentMode::Mailbox;
            case VK_PRESENT_MODE_IMMEDIATE_KHR: return RHI::PresentMode::Immediate;
            default: return RHI::PresentMode::Other;
        }
    }

    RHI::FrameStatus RhiDevice::BeginFrame() {
        if (!m_swapchain) {
            return RHI::FrameStatus::Error; // sans fenêtre : Submit seulement
        }
        using Clock = std::chrono::steady_clock;
        const auto milliseconds = [](Clock::duration duration) { return std::chrono::duration<double, std::milli>(duration).count(); };
        Frame& frame = m_frames[m_frameIndex];
        const VkDevice device = m_device->GetHandle();
        const Clock::time_point waitStart = Clock::now();
        if (!CheckFrame(vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences")) {
            return RHI::FrameStatus::Error;
        }
        const Clock::time_point acquireStart = Clock::now();
        m_waitTimes.frame = milliseconds(acquireStart - waitStart);
        ReadTimestamps(frame);
        m_device->ReleaseUploads(frame.uploads); // finis avec l'image

        const VkResult acquire = vkAcquireNextImageKHR(device, m_swapchain->GetHandle(), UINT64_MAX, frame.imageAvailable,
                                                       VK_NULL_HANDLE, &m_imageIndex);
        m_waitTimes.acquire = milliseconds(Clock::now() - acquireStart);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            return RecreateSwapchain() ? RHI::FrameStatus::Skip : RHI::FrameStatus::Error; // on retentera à l'image suivante
        }
        if (!CheckFrame(acquire, "vkAcquireNextImageKHR")) {
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
                            m_swapchain->GetExtent(), frame.queries);
        frame.submitted = false;
        if (m_stepMarkers.GetHandle() != VK_NULL_HANDLE) {
            // Numéro jamais nul : le buffer commence à zéro, une étape jamais atteinte ne lui ressemble pas.
            m_frameSerial = m_frameSerial == UINT32_MAX ? 1 : m_frameSerial + 1;
            frame.serial = m_frameSerial;
            m_commandList.SetStepMarkers({m_stepMarkers.GetHandle(), m_frameIndex * RHI::MaxSteps * 2 * sizeof(std::uint32_t),
                                          frame.serial, &frame.steps, &frame.stepCount});
        }
        return RHI::FrameStatus::Ready;
    }

    bool RhiDevice::EndFrame() {
        Frame& frame = m_frames[m_frameIndex];
        m_commandList.EndStep();
        m_commandList.TransitionToPresent();
        frame.writtenTimestamps = m_commandList.GetWrittenTimestamps();
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

        // Les envois enregistrés pendant la préparation de l'image d'abord : elle dessine peut-être ce qu'ils copient.
        frame.uploads = m_device->TakeUploads();
        std::array<VkCommandBufferSubmitInfo, 2> commandInfos{};
        std::uint32_t commandCount = 0;
        for (VkCommandBuffer commands : {frame.uploads.commands, frame.commandBuffer}) {
            if (commands != VK_NULL_HANDLE) {
                commandInfos[commandCount].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
                commandInfos[commandCount].commandBuffer = commands;
                ++commandCount;
            }
        }

        VkSubmitInfo2 submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submitInfo.waitSemaphoreInfoCount = 1;
        submitInfo.pWaitSemaphoreInfos = &waitInfo;
        submitInfo.commandBufferInfoCount = commandCount;
        submitInfo.pCommandBufferInfos = commandInfos.data();
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos = &signalInfo;
        if (!CheckFrame(vkQueueSubmit2(m_device->GetGraphicsQueue(), 1, &submitInfo, frame.inFlight), "vkQueueSubmit2")) {
            return false;
        }
        frame.submitted = true;

        const VkSwapchainKHR swapchain = m_swapchain->GetHandle();
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &m_imageIndex;
        const auto presentStart = std::chrono::steady_clock::now();
        const VkResult present = vkQueuePresentKHR(m_device->GetPresentQueue(), &presentInfo);
        m_waitTimes.present = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentStart).count();

        m_frameIndex = (m_frameIndex + 1) % RHI::FramesInFlight;

        if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR) {
            return RecreateSwapchain();
        }
        return CheckFrame(present, "vkQueuePresentKHR");
    }

    bool RhiDevice::CheckFrame(VkResult result, const char* call) {
        if (result == VK_ERROR_DEVICE_LOST) {
            ReportDeviceLost();
        }
        return Check(result, call);
    }

    void RhiDevice::ReportDeviceLost() {
        if (m_lostReported) {
            return;
        }
        m_lostReported = true;
        std::string report = std::format("Le GPU s'est bloqué ou a été relancé par le pilote ({}), Flux {}.", m_device->GetName(),
                                         GetFullVersion());
        if (m_stepMarkers.GetHandle() == VK_NULL_HANDLE) {
            report += "\nÉtapes inconnues : ce GPU ne note pas où il en est (pas de VK_AMD_buffer_marker).";
        } else {
            // Images envoyées, de la plus ancienne à la plus récente : la première étape commencée mais pas finie est
            // celle où le GPU s'est arrêté.
            std::array<const Frame*, RHI::FramesInFlight> frames{};
            for (std::uint32_t i = 0; i < RHI::FramesInFlight; ++i) {
                frames[i] = &m_frames[i];
            }
            std::ranges::sort(frames, {}, [](const Frame* frame) { return frame->serial; });
            std::array<std::uint32_t, RHI::FramesInFlight * RHI::MaxSteps * 2> marks{};
            m_stepMarkers.Read(marks.data(), sizeof(marks));
            for (const Frame* frame : frames) {
                if (!frame->submitted || frame->stepCount == 0) {
                    continue;
                }
                const std::size_t first = static_cast<std::size_t>(frame - m_frames.data()) * RHI::MaxSteps * 2;
                report += std::format("\nImage {} :", frame->serial);
                for (std::uint32_t step = 0; step < frame->stepCount; ++step) {
                    const bool started = marks[first + step * 2] == frame->serial;
                    const bool finished = marks[first + step * 2 + 1] == frame->serial;
                    report += std::format("\n  {:<28} {}", frame->steps[step],
                                          finished ? "finie" : started ? "COMMENCÉE, PAS FINIE" : "pas commencée");
                }
            }
        }
        if (const std::string fault = m_device->DescribeFault(); !fault.empty()) {
            report += "\nPanne décrite par le pilote : " + fault;
        }
        Log::Error("{}", report);
        // Le programme s'arrête juste après : le rapport reste dans un fichier à côté de l'exe.
        const std::filesystem::path file = Platform::GetExecutableDirectory() / "plantage-gpu.txt";
        if (std::ofstream out(file, std::ios::binary); out << report << '\n') {
            Log::Error("Rapport écrit dans {}", file.string());
        }
    }
}
