#include "Renderer/Vulkan/VulkanPipeline.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <utility>
#include <vector>

namespace Flux::Vk {
    namespace {
        std::expected<std::vector<uint32_t>, std::string> ReadSpirv(const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) {
                return std::unexpected("shader introuvable : " + path.string());
            }
            const auto size = static_cast<std::size_t>(file.tellg());
            if (size == 0 || size % sizeof(uint32_t) != 0) {
                return std::unexpected("fichier SPIR-V invalide : " + path.string());
            }
            std::vector<uint32_t> code(size / sizeof(uint32_t));
            file.seekg(0);
            file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));
            return code;
        }

        std::expected<VkShaderModule, std::string> CreateModule(VkDevice device, const std::filesystem::path& path) {
            auto code = ReadSpirv(path);
            if (!code) {
                return std::unexpected(code.error());
            }
            VkShaderModuleCreateInfo moduleInfo{};
            moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            moduleInfo.codeSize = code->size() * sizeof(uint32_t);
            moduleInfo.pCode = code->data();
            VkShaderModule shaderModule = VK_NULL_HANDLE;
            if (!Check(vkCreateShaderModule(device, &moduleInfo, nullptr, &shaderModule), "vkCreateShaderModule")) {
                return std::unexpected("création du module de " + path.string());
            }
            return shaderModule;
        }

        VkResult CreateLayout(VkDevice device, std::span<const VkDescriptorSetLayout> setLayouts,
                              std::span<const VkPushConstantRange> pushConstants, VkPipelineLayout& layout) {
            VkPipelineLayoutCreateInfo layoutInfo{};
            layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
            layoutInfo.pSetLayouts = setLayouts.data();
            layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size());
            layoutInfo.pPushConstantRanges = pushConstants.data();
            return vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout);
        }
    }

    std::expected<Pipeline, std::string> Pipeline::CreateCompute(VkDevice device, const ComputePipelineDesc& desc) {
        auto shaderModule = CreateModule(device, desc.shaderPath);
        if (!shaderModule) {
            return std::unexpected(shaderModule.error());
        }
        Pipeline pipeline;
        pipeline.m_device = device;
        pipeline.m_bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
        pipeline.m_pushStages = VK_SHADER_STAGE_COMPUTE_BIT;
        const VkPushConstantRange pushConstants{VK_SHADER_STAGE_COMPUTE_BIT, 0, desc.pushConstantsSize};
        const std::span<const VkPushConstantRange> ranges(&pushConstants, desc.pushConstantsSize > 0 ? 1 : 0);
        if (!Check(CreateLayout(device, desc.setLayouts, ranges, pipeline.m_layout), "vkCreatePipelineLayout")) {
            vkDestroyShaderModule(device, *shaderModule, nullptr);
            return std::unexpected("création du pipeline layout");
        }

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = *shaderModule;
        pipelineInfo.stage.pName = desc.entryPoint;
        pipelineInfo.layout = pipeline.m_layout;
        const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline.m_pipeline);
        vkDestroyShaderModule(device, *shaderModule, nullptr);
        if (!Check(result, "vkCreateComputePipelines")) {
            return std::unexpected("création du pipeline de calcul pour " + desc.shaderPath.string());
        }
        return pipeline;
    }

    std::expected<Pipeline, std::string> Pipeline::CreateGraphics(VkDevice device, const GraphicsPipelineDesc& desc) {
        auto module = CreateModule(device, desc.shaderPath);
        if (!module) {
            return std::unexpected(module.error());
        }
        const VkShaderModule shaderModule = *module;

        Pipeline pipeline;
        pipeline.m_device = device;
        pipeline.m_pushStages = desc.pushConstants.empty() ? 0 : desc.pushConstants.front().stageFlags;
        if (!Check(CreateLayout(device, desc.setLayouts, desc.pushConstants, pipeline.m_layout), "vkCreatePipelineLayout")) {
            vkDestroyShaderModule(device, shaderModule, nullptr);
            return std::unexpected("création du pipeline layout");
        }

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = shaderModule;
        stages[0].pName = desc.vertexEntryPoint;
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = shaderModule;
        stages[1].pName = desc.fragmentEntryPoint;

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(desc.vertexBindings.size());
        vertexInput.pVertexBindingDescriptions = desc.vertexBindings.data();
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(desc.vertexAttributes.size());
        vertexInput.pVertexAttributeDescriptions = desc.vertexAttributes.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        // Les tailles réelles sont données à chaque frame (vkCmdSetViewport / vkCmdSetScissor).
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterization{};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = desc.cullMode;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;
        if (desc.depthBiasConstant != 0.0f || desc.depthBiasSlope != 0.0f) {
            rasterization.depthBiasEnable = VK_TRUE;
            rasterization.depthBiasConstantFactor = desc.depthBiasConstant;
            rasterization.depthBiasSlopeFactor = desc.depthBiasSlope;
        }

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = desc.depthCompare;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (desc.alphaBlend) {
            blendAttachment.blendEnable = VK_TRUE;
            blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        } else if (desc.additiveBlend) {
            blendAttachment.blendEnable = VK_TRUE;
            blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        }

        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        const bool hasColor = desc.colorFormat != VK_FORMAT_UNDEFINED; // sinon : profondeur seule
        colorBlend.attachmentCount = hasColor ? 1 : 0;
        colorBlend.pAttachments = &blendAttachment;

        constexpr std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        // Dynamic rendering : on déclare les formats des attachments au lieu d'un VkRenderPass.
        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.colorAttachmentCount = hasColor ? 1 : 0;
        renderingInfo.pColorAttachmentFormats = &desc.colorFormat;
        renderingInfo.depthAttachmentFormat = desc.depthFormat;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = desc.depthFormat != VK_FORMAT_UNDEFINED ? &depthStencil : nullptr;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = pipeline.m_layout;

        const VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline.m_pipeline);
        // Le module n'est plus nécessaire une fois le pipeline créé (ou raté).
        vkDestroyShaderModule(device, shaderModule, nullptr);
        if (!Check(result, "vkCreateGraphicsPipelines")) {
            return std::unexpected("création du pipeline pour " + desc.shaderPath.string());
        }
        return pipeline;
    }

    Pipeline::~Pipeline() {
        Reset();
    }

    Pipeline::Pipeline(Pipeline&& other) noexcept
        : m_device(std::exchange(other.m_device, VK_NULL_HANDLE)),
          m_layout(std::exchange(other.m_layout, VK_NULL_HANDLE)),
          m_pipeline(std::exchange(other.m_pipeline, VK_NULL_HANDLE)),
          m_bindPoint(other.m_bindPoint),
          m_pushStages(other.m_pushStages) {}

    Pipeline& Pipeline::operator=(Pipeline&& other) noexcept {
        if (this != &other) {
            Reset();
            m_device = std::exchange(other.m_device, VK_NULL_HANDLE);
            m_layout = std::exchange(other.m_layout, VK_NULL_HANDLE);
            m_pipeline = std::exchange(other.m_pipeline, VK_NULL_HANDLE);
            m_bindPoint = other.m_bindPoint;
            m_pushStages = other.m_pushStages;
        }
        return *this;
    }

    void Pipeline::Reset() {
        if (m_device != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
            vkDestroyPipelineLayout(m_device, m_layout, nullptr);
        }
        m_pipeline = VK_NULL_HANDLE;
        m_layout = VK_NULL_HANDLE;
    }
}
