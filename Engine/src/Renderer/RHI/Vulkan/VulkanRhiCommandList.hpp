#pragma once

// RHI::CommandList sur un command buffer Vulkan.

#include "Flux/Renderer/RHI/CommandList.hpp"
#include "Flux/Renderer/RHI/RHITypes.hpp"
#include "Renderer/RHI/Vulkan/Vulkan.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace Flux::Vk {
    // Repères d'étapes d'une image (BeginStep) : noms gardés par le CPU, début et fin notés par le GPU dans `buffer`
    // (deux uint32 par étape à partir de `offset`, égaux à `serial` une fois atteints).
    struct StepMarkers
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        std::uint32_t serial = 0;
        std::array<const char*, RHI::MaxSteps>* names = nullptr;
        std::uint32_t* count = nullptr;
    };

    // Liste de commandes de l'image en cours, et l'image de la swapchain où dessine sa passe.
    class RhiCommandList final : public RHI::CommandList {
        public:
        // queryPool : heures notées par WriteTimestamp (VK_NULL_HANDLE : pas d'heures), remise à zéro ici.
        // image VK_NULL_HANDLE : commandes hors des images (Device::Submit), sans passe vers la swapchain.
        void Begin(VkCommandBuffer commandBuffer, VkImage image, VkImageView view, VkExtent2D extent, VkQueryPool queryPool);
        // Termine la passe vers la présentation : l'image de la swapchain passe en lecture pour l'écran.
        void TransitionToPresent();
        // Copie l'image de la swapchain, telle que dessinée jusqu'ici, dans target (Readback, largeur x hauteur x 4
        // octets) : capture d'écran. Avant TransitionToPresent.
        void CopyImageToReadback(VkBuffer target);
        // Pour une bibliothèque qui enregistre elle-même ses commandes (FSR).
        VkCommandBuffer GetHandle() const { return m_commandBuffer; }

        void UseTextures(std::span<const RHI::TextureUse> uses) override;
        void BeginRenderPass(const RHI::RenderPassDesc& desc) override;
        void EndRenderPass() override;
        void SetViewport(const RHI::Viewport& viewport) override;
        void SetScissor(const RHI::Rect& rect) override;
        void ClearDepth(const RHI::Rect& rect, float depth) override;
        void BindPipeline(const RHI::Pipeline& pipeline) override;
        void SetBindGroup(std::uint32_t index, const RHI::BindGroup& group) override;
        void BindVertexBuffers(std::uint32_t first, std::span<const RHI::Buffer* const> buffers) override;
        void BindIndexBuffer(const RHI::Buffer& buffer) override;
        void PushConstants(const void* data, std::uint32_t size) override;
        void Draw(std::uint32_t vertexCount, std::uint32_t firstVertex) override;
        void DrawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex, std::uint32_t instance) override;
        void DrawIndexedIndirect(const RHI::Buffer& buffer, std::uint64_t offset, std::uint32_t count) override;
        void DrawMeshTasks(std::uint32_t groups) override;
        void Dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) override;
        void ComputeBarrier() override;
        void GenerateMipmaps(const RHI::Texture& texture) override;
        void CopyBuffer(const RHI::Buffer& source, std::uint64_t sourceOffset, RHI::Buffer& target, std::uint64_t targetOffset,
                        std::uint64_t size) override;
        void WriteTimestamp(std::uint32_t slot) override;
        void BeginStep(const char* name) override;
        // Repères de l'image qui commence (après Begin) ; sans appel, BeginStep ne fait rien.
        void SetStepMarkers(const StepMarkers& markers);
        // VK_EXT_debug_utils activée (Device::HasDebugLabels) : chaque étape devient aussi un repère nommé.
        void SetDebugLabels(bool enabled) { m_debugLabels = enabled; }
        // Mesh shaders activés : les barrières attendent aussi les étapes de tâche et de maillage.
        void SetMeshShaders(bool enabled) { m_meshShaders = enabled; }
        // Note la fin de l'étape en cours, s'il y en a une (fin de l'image).
        void EndStep();
        // Cases écrites pendant l'image (bit i : case i).
        std::uint32_t GetWrittenTimestamps() const { return m_writtenTimestamps; }

        private:
        void BeginDepthOnlyPass(const RHI::RenderPassDesc& desc);
        void BeginTexturePass(const RHI::RenderPassDesc& desc);
        VkRenderingAttachmentInfo BeginDepthAttachment(const RHI::RenderPassDesc& desc);

        VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
        VkImage m_image = VK_NULL_HANDLE;
        VkImageView m_view = VK_NULL_HANDLE;
        VkExtent2D m_extent{};
        VkPipelineLayout m_layout = VK_NULL_HANDLE; // du pipeline lié
        VkPipelineBindPoint m_bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        VkShaderStageFlags m_pushStages = 0;
        VkQueryPool m_queryPool = VK_NULL_HANDLE;
        std::uint32_t m_writtenTimestamps = 0;
        StepMarkers m_markers;
        std::uint32_t m_openStep = RHI::MaxSteps; // MaxSteps : aucune
        bool m_debugLabels = false;
        bool m_meshShaders = false;
        bool m_labelOpen = false;                 // repère VK_EXT_debug_utils ouvert par BeginStep
    };
}
