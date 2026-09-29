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
    }

    std::expected<GraphicsPipeline, std::string> GraphicsPipeline::Create(VkDevice device, const GraphicsPipelineDesc& desc) {
        auto code = ReadSpirv(desc.shaderPath);
        if (!code) {
            return std::unexpected(code.error());
        }

        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = code->size() * sizeof(uint32_t);
        moduleInfo.pCode = code->data();
        VkShaderModule shaderModule = VK_NULL_HANDLE;
        if (!Check(vkCreateShaderModule(device, &moduleInfo, nullptr, &shaderModule), "vkCreateShaderModule")) {
            return std::unexpected("création du module de " + desc.shaderPath.string());
        }

        GraphicsPipeline pipeline;
        pipeline.m_device = device;

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = static_cast<uint32_t>(desc.setLayouts.size());
        layoutInfo.pSetLayouts = desc.setLayouts.data();
        layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(desc.pushConstants.size());
        layoutInfo.pPushConstantRanges = desc.pushConstants.data();
        if (!Check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipeline.m_layout), "vkCreatePipelineLayout")) {
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
        }

        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blendAttachment;

        constexpr std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        // Dynamic rendering : on déclare les formats des attachments au lieu d'un VkRenderPass.
        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.colorAttachmentCount = 1;
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

    GraphicsPipeline::~GraphicsPipeline() {
        Reset();
    }

    GraphicsPipeline::GraphicsPipeline(GraphicsPipeline&& other) noexcept
        : m_device(std::exchange(other.m_device, VK_NULL_HANDLE)),
          m_layout(std::exchange(other.m_layout, VK_NULL_HANDLE)),
          m_pipeline(std::exchange(other.m_pipeline, VK_NULL_HANDLE)) {}

    GraphicsPipeline& GraphicsPipeline::operator=(GraphicsPipeline&& other) noexcept {
        if (this != &other) {
            Reset();
            m_device = std::exchange(other.m_device, VK_NULL_HANDLE);
            m_layout = std::exchange(other.m_layout, VK_NULL_HANDLE);
            m_pipeline = std::exchange(other.m_pipeline, VK_NULL_HANDLE);
        }
        return *this;
    }

    void GraphicsPipeline::Reset() {
        if (m_device != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
            vkDestroyPipelineLayout(m_device, m_layout, nullptr);
        }
        m_pipeline = VK_NULL_HANDLE;
        m_layout = VK_NULL_HANDLE;
    }
}
