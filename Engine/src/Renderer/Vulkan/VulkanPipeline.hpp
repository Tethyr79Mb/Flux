#pragma once

#include "Renderer/Vulkan/Vulkan.hpp"

#include <expected>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace Flux::Vk {
    struct GraphicsPipelineDesc {
        // Module SPIR-V contenant les deux points d'entrée (compilé par Slang).
        std::filesystem::path shaderPath;
        const char* vertexEntryPoint = "vertexMain";
        const char* fragmentEntryPoint = "fragmentMain";

        std::span<const VkVertexInputBindingDescription> vertexBindings;
        std::span<const VkVertexInputAttributeDescription> vertexAttributes;

        std::span<const VkDescriptorSetLayout> setLayouts;
        std::span<const VkPushConstantRange> pushConstants;

        VkFormat colorFormat = VK_FORMAT_UNDEFINED;
        // Doit correspondre à la passe, même si ce pipeline ne teste pas la profondeur.
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        bool depthTest = false;
        bool depthWrite = false;
        VkCompareOp depthCompare = VK_COMPARE_OP_GREATER; // reverse-Z : plus proche = plus grand

        // Les maillages sont en sens antihoraire vu de l'extérieur.
        VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
        // Mélange alpha classique (couleur non prémultipliée), pour l'UI et le texte.
        bool alphaBlend = false;
        // Décalage de profondeur (cartes d'ombres) ; 0 et 0 : aucun.
        float depthBiasConstant = 0.0f;
        float depthBiasSlope = 0.0f;
    };

    struct ComputePipelineDesc {
        std::filesystem::path shaderPath;
        const char* entryPoint = "computeMain";
        std::span<const VkDescriptorSetLayout> setLayouts;
        std::uint32_t pushConstantsSize = 0;
    };

    // Pipeline graphique (dynamic rendering ; viewport et scissor dynamiques) ou de calcul.
    class Pipeline {
        public:
        static std::expected<Pipeline, std::string> CreateGraphics(VkDevice device, const GraphicsPipelineDesc& desc);
        static std::expected<Pipeline, std::string> CreateCompute(VkDevice device, const ComputePipelineDesc& desc);

        Pipeline() = default;
        ~Pipeline();

        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;
        Pipeline(Pipeline&& other) noexcept;
        Pipeline& operator=(Pipeline&& other) noexcept;

        VkPipeline GetHandle() const { return m_pipeline; }
        VkPipelineLayout GetLayout() const { return m_layout; }
        VkPipelineBindPoint GetBindPoint() const { return m_bindPoint; }
        // Étapes qui voient les push constants.
        VkShaderStageFlags GetPushStages() const { return m_pushStages; }

        private:
        void Reset();

        VkDevice m_device = VK_NULL_HANDLE;
        VkPipelineLayout m_layout = VK_NULL_HANDLE;
        VkPipeline m_pipeline = VK_NULL_HANDLE;
        VkPipelineBindPoint m_bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        VkShaderStageFlags m_pushStages = 0;
    };
}
