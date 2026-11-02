#include "Renderer/RHI/Vulkan/VulkanRHI.hpp"

#include "Renderer/RHI/Vulkan/VulkanUpscaler.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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
        rhi->m_commandList.SetDebugLabels(rhi->m_device->HasDebugLabels());
        rhi->m_commandList.SetMeshShaders(rhi->m_device->HasMeshShaders());
        rhi->m_timestamps = std::make_unique<Timestamps>(*rhi->m_device);
        rhi->m_crash = std::make_unique<CrashMarkers>(*rhi->m_device, nativeWindowHandle != nullptr);
        rhi->m_frames = std::make_unique<Frames>(*rhi->m_device);
        if (!rhi->m_frames->Create()) {
            return std::unexpected("création des ressources de frame");
        }
        return rhi;
    }

    RhiDevice::~RhiDevice() {
        if (!m_device) {
            return;
        }
        m_device->WaitIdle();
        RunRetired(true);
        // Puis les membres, avant le device (ordre inverse de leur déclaration).
    }

    std::span<const double> RhiDevice::GetTimestamps() const {
        return m_timestamps ? m_timestamps->Get() : std::span<const double>{};
    }

    std::expected<std::unique_ptr<RHI::Buffer>, std::string> RhiDevice::CreateBuffer(const RHI::BufferDesc& desc, const void* data) {
        const VkBufferUsageFlags usage = ToVulkan(desc.usage);
        // Source et destination de copie (CommandList::CopyBuffer, envois) : sans coût pour le GPU.
        const VkBufferUsageFlags copyUsage = usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (desc.memory == RHI::MemoryType::Readback) {
            auto buffer = Vk::Buffer::Create(m_device->GetAllocator(), desc.size, copyUsage, Vk::Buffer::Memory::Readback);
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

        // Mémoire vidéo, remplie par la file de transfert (partagée avec elle : pas de transfert de propriété).
        auto buffer = Vk::Buffer::Create(m_device->GetAllocator(), desc.size, copyUsage, Vk::Buffer::Memory::GpuOnly,
                                         m_device->GetSharedFamilies());
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        if (data && !m_device->UploadBuffer(buffer->GetHandle(), 0, data, desc.size, false)) {
            return std::unexpected("envoi d'un buffer au GPU");
        }
        return std::make_unique<RhiBuffer>(std::move(*buffer));
    }

    bool RhiDevice::WriteBuffer(RHI::Buffer& buffer, std::uint64_t offset, const void* data, std::uint64_t size, bool inUse) {
        auto& target = static_cast<RhiBuffer&>(buffer);
        if (size == 0) {
            return true;
        }
        if (target.IsMapped()) {
            target.Write(data, size, offset);
            return true;
        }
        return m_device->UploadBuffer(target.GetHandle(), offset, data, size, inUse);
    }

    std::expected<std::unique_ptr<RHI::Texture>, std::string> RhiDevice::CreateTexture(RHI::Format format, RHI::Extent extent,
                                                                                      const RHI::StagingBuffer& source,
                                                                                      std::uint64_t offset, bool mipmaps) {
        const auto& staging = static_cast<const RhiStagingBuffer&>(source);
        if (format != RHI::Format::RGBA8Unorm && format != RHI::Format::RGBA8Srgb) {
            return std::unexpected("texture depuis une mémoire d'envoi : RGBA8 seulement");
        }
        if (offset + std::uint64_t{extent.width} * extent.height * 4 > staging.GetSize()) {
            return std::unexpected("texture depuis une mémoire d'envoi : pixels hors de la mémoire");
        }
        auto image = Image::CreateTexture(*m_device, ToVulkan(format), {extent.width, extent.height}, staging.GetHandle(), offset, mipmaps);
        if (!image) {
            return std::unexpected(image.error());
        }
        return std::make_unique<RhiTexture>(std::move(*image), format, VK_IMAGE_ASPECT_COLOR_BIT, RHI::TextureAccess::Sampled);
    }

    std::expected<std::unique_ptr<RHI::StagingBuffer>, std::string> RhiDevice::CreateStagingBuffer(std::uint64_t size) {
        // VMA est sûr entre fils (allocateur créé sans VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT).
        auto buffer = Vk::Buffer::Create(m_device->GetAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Vk::Buffer::Memory::Staging);
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        return std::make_unique<RhiStagingBuffer>(std::move(*buffer));
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

    std::expected<std::unique_ptr<RHI::Texture>, std::string> RhiDevice::CreateVolumeTexture(RHI::Format format, RHI::Extent extent,
                                                                                             std::uint32_t depth, bool mipmaps) {
        if (format != RHI::Format::RGBA8Unorm) {
            return std::unexpected("texture 3D : format RGBA8Unorm seulement");
        }
        auto image = Image::CreateVolume(*m_device, ToVulkan(format), {extent.width, extent.height}, depth, mipmaps);
        if (!image) {
            return std::unexpected(image.error());
        }
        return std::make_unique<RhiTexture>(std::move(*image), format, VK_IMAGE_ASPECT_COLOR_BIT, RHI::TextureAccess::None);
    }

    std::expected<std::unique_ptr<RHI::Texture>, std::string> RhiDevice::CreateRenderTarget(RHI::Format format, RHI::Extent extent,
                                                                                            bool sampled, bool storage) {
        const bool depth = format == RHI::Format::D32Float;
        const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageUsageFlags usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (sampled) {
            usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        }
        if (storage && !depth) {
            usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }
        auto image = Image::CreateAttachment(*m_device, ToVulkan(format), {extent.width, extent.height}, usage, aspect);
        if (!image) {
            return std::unexpected(image.error());
        }
        return std::make_unique<RhiTexture>(std::move(*image), format, aspect, RHI::TextureAccess::None);
    }

    std::expected<std::unique_ptr<RHI::Upscaler>, std::string> RhiDevice::CreateUpscaler(RHI::Extent outputSize) {
        return CreateFsrUpscaler(*m_device, outputSize);
    }

    std::expected<std::unique_ptr<RHI::Texture>, std::string> RhiDevice::CreateCubeRenderTarget(RHI::Format format,
                                                                                                std::uint32_t size,
                                                                                                std::uint32_t mipLevels) {
        VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (mipLevels > 1) {
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; // GenerateMipmaps
        }
        auto image = Image::CreateCubeAttachment(*m_device, ToVulkan(format), size, usage, mipLevels);
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
            bindings.push_back({binding.slot, type, count, ToVulkan(binding.stages, m_device->HasMeshShaders()), nullptr});
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
        if (desc.meshEntryPoint && !m_device->HasMeshShaders()) {
            return std::unexpected("mesh shaders demandés, absents de ce GPU : " + desc.shaderPath.string());
        }
        const VkPushConstantRange pushConstants{
            ToVulkan(RHI::ShaderStage::Vertex | RHI::ShaderStage::Fragment, m_device->HasMeshShaders()), 0, desc.pushConstantsSize};

        GraphicsPipelineDesc vkDesc;
        vkDesc.shaderPath = desc.shaderPath;
        vkDesc.vertexEntryPoint = desc.vertexEntryPoint;
        vkDesc.fragmentEntryPoint = desc.fragmentEntryPoint;
        vkDesc.taskEntryPoint = desc.taskEntryPoint;
        vkDesc.meshEntryPoint = desc.meshEntryPoint;
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
        vkDesc.cullMode = ToVulkan(desc.cullMode);
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
}
