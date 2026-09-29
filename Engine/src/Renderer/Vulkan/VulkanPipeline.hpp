#pragma once

#include "Renderer/Vulkan/Vulkan.hpp"

#include <expected>
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
    };

    // Pipeline graphique pour le dynamic rendering ; viewport et scissor sont dynamiques.
    class GraphicsPipeline {
        public:
        static std::expected<GraphicsPipeline, std::string> Create(VkDevice device, const GraphicsPipelineDesc& desc);

        GraphicsPipeline() = default;
        ~GraphicsPipeline();

        GraphicsPipeline(const GraphicsPipeline&) = delete;
        GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
        GraphicsPipeline(GraphicsPipeline&& other) noexcept;
        GraphicsPipeline& operator=(GraphicsPipeline&& other) noexcept;

        VkPipeline GetHandle() const { return m_pipeline; }
        VkPipelineLayout GetLayout() const { return m_layout; }

        private:
        void Reset();

        VkDevice m_device = VK_NULL_HANDLE;
        VkPipelineLayout m_layout = VK_NULL_HANDLE;
        VkPipeline m_pipeline = VK_NULL_HANDLE;
    };
}
