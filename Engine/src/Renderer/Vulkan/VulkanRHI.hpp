#pragma once

// Backend Vulkan du RHI : chaque interface de Renderer/RHI/RHI.hpp y est implémentée sur les enveloppes Vk::
// (Device, Swapchain, Buffer, Image, Pipeline).

#include "Renderer/RHI/RHI.hpp"
#include "Renderer/Vulkan/VulkanBuffer.hpp"
#include "Renderer/Vulkan/VulkanDevice.hpp"
#include "Renderer/Vulkan/VulkanImage.hpp"
#include "Renderer/Vulkan/VulkanPipeline.hpp"
#include "Renderer/Vulkan/VulkanSwapchain.hpp"

#include <array>
#include <vector>

namespace Flux::Vk {
    VkFormat ToVulkan(RHI::Format format);
    RHI::Format FromVulkan(VkFormat format);

    class RhiBuffer final : public RHI::Buffer {
        public:
        explicit RhiBuffer(Vk::Buffer buffer) : m_buffer(std::move(buffer)) {}
        std::uint64_t GetSize() const override { return m_buffer.GetSize(); }
        void Write(const void* data, std::uint64_t size, std::uint64_t offset) override { m_buffer.Write(data, size, offset); }
        void Read(void* data, std::uint64_t size, std::uint64_t offset) const override { m_buffer.Read(data, size, offset); }
        VkBuffer GetHandle() const { return m_buffer.GetHandle(); }

        private:
        Vk::Buffer m_buffer;
    };

    class RhiTexture final : public RHI::Texture {
        public:
        RhiTexture(Image image, RHI::Format format, VkImageAspectFlags aspect)
            : m_image(std::move(image)), m_format(format), m_aspect(aspect) {}
        RHI::Extent GetExtent() const override { return {m_image.GetExtent().width, m_image.GetExtent().height}; }
        RHI::Format GetFormat() const override { return m_format; }
        const Image& GetImage() const { return m_image; }
        VkImageAspectFlags GetAspect() const { return m_aspect; }

        private:
        Image m_image;
        RHI::Format m_format;
        VkImageAspectFlags m_aspect;
    };

    class RhiSampler final : public RHI::Sampler {
        public:
        RhiSampler(VkDevice device, VkSampler sampler) : m_device(device), m_sampler(sampler) {}
        ~RhiSampler() override { vkDestroySampler(m_device, m_sampler, nullptr); }
        RhiSampler(const RhiSampler&) = delete;
        RhiSampler& operator=(const RhiSampler&) = delete;
        VkSampler GetHandle() const { return m_sampler; }

        private:
        VkDevice m_device;
        VkSampler m_sampler;
    };

    // Le layout et son pool : ses groupes y sont pris, et y retournent un par un.
    class RhiBindGroupLayout final : public RHI::BindGroupLayout {
        public:
        RhiBindGroupLayout(VkDevice device, VkDescriptorSetLayout layout, VkDescriptorPool pool)
            : m_device(device), m_layout(layout), m_pool(pool) {}
        ~RhiBindGroupLayout() override;
        RhiBindGroupLayout(const RhiBindGroupLayout&) = delete;
        RhiBindGroupLayout& operator=(const RhiBindGroupLayout&) = delete;
        VkDescriptorSetLayout GetHandle() const { return m_layout; }
        VkDescriptorPool GetPool() const { return m_pool; }

        private:
        VkDevice m_device;
        VkDescriptorSetLayout m_layout;
        VkDescriptorPool m_pool;
    };

    class RhiBindGroup final : public RHI::BindGroup {
        public:
        RhiBindGroup(VkDevice device, VkDescriptorPool pool, VkDescriptorSet set) : m_device(device), m_pool(pool), m_set(set) {}
        ~RhiBindGroup() override { vkFreeDescriptorSets(m_device, m_pool, 1, &m_set); }
        RhiBindGroup(const RhiBindGroup&) = delete;
        RhiBindGroup& operator=(const RhiBindGroup&) = delete;
        void SetTexture(std::uint32_t slot, const RHI::Texture& texture, const RHI::Sampler& sampler) override;
        void SetBuffer(std::uint32_t slot, const RHI::Buffer& buffer) override;
        VkDescriptorSet GetHandle() const { return m_set; }

        private:
        VkDevice m_device;
        VkDescriptorPool m_pool;
        VkDescriptorSet m_set;
    };

    class RhiPipeline final : public RHI::Pipeline {
        public:
        explicit RhiPipeline(Vk::Pipeline pipeline) : m_pipeline(std::move(pipeline)) {}
        const Vk::Pipeline& Get() const { return m_pipeline; }

        private:
        Vk::Pipeline m_pipeline;
    };

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

        void BeginRenderPass(const RHI::RenderPassDesc& desc) override;
        void EndRenderPass() override;
        void SetViewport(const RHI::Viewport& viewport) override;
        void SetScissor(const RHI::Rect& rect) override;
        void BindPipeline(const RHI::Pipeline& pipeline) override;
        void SetBindGroup(std::uint32_t index, const RHI::BindGroup& group) override;
        void BindVertexBuffers(std::uint32_t first, std::span<const RHI::Buffer* const> buffers) override;
        void BindIndexBuffer(const RHI::Buffer& buffer) override;
        void PushConstants(const void* data, std::uint32_t size) override;
        void Draw(std::uint32_t vertexCount, std::uint32_t firstVertex) override;
        void DrawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex, std::uint32_t instance) override;
        void DrawIndexedIndirect(const RHI::Buffer& buffer, std::uint64_t offset, std::uint32_t count) override;
        void Dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) override;
        void ComputeBarrier() override;
        void WriteTimestamp(std::uint32_t slot) override;
        void BeginStep(const char* name) override;
        // Repères de l'image qui commence (après Begin) ; sans appel, BeginStep ne fait rien.
        void SetStepMarkers(const StepMarkers& markers);
        // Note la fin de l'étape en cours, s'il y en a une (fin de l'image).
        void EndStep();
        // Cases écrites pendant l'image (bit i : case i).
        std::uint32_t GetWrittenTimestamps() const { return m_writtenTimestamps; }

        private:
        void BeginDepthOnlyPass(const RHI::RenderPassDesc& desc);
        void BeginTexturePass(const RHI::RenderPassDesc& desc);

        VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
        VkImage m_image = VK_NULL_HANDLE;
        VkImageView m_view = VK_NULL_HANDLE;
        VkExtent2D m_extent{};
        VkPipelineLayout m_layout = VK_NULL_HANDLE; // du pipeline lié
        VkPipelineBindPoint m_bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        VkShaderStageFlags m_pushStages = 0;
        const RhiTexture* m_depthOnly = nullptr;    // passe de profondeur seule en cours : lisible à sa fin
        const RhiTexture* m_colorTarget = nullptr;  // passe vers une face de texture cube : lisible à sa fin
        std::uint32_t m_colorLayer = 0;
        VkQueryPool m_queryPool = VK_NULL_HANDLE;
        std::uint32_t m_writtenTimestamps = 0;
        StepMarkers m_markers;
        std::uint32_t m_openStep = RHI::MaxSteps; // MaxSteps : aucune
    };

    class RhiDevice final : public RHI::Device {
        public:
        // nativeWindowHandle nul : sans fenêtre ni swapchain (Submit seulement).
        static std::expected<std::unique_ptr<RhiDevice>, std::string> Create(void* nativeWindowHandle, RHI::Extent extent, bool vsync);
        ~RhiDevice() override;

        std::string_view GetName() const override { return m_device->GetName(); }
        void WaitIdle() override { m_device->WaitIdle(); }

        std::expected<std::unique_ptr<RHI::Buffer>, std::string> CreateBuffer(const RHI::BufferDesc& desc, const void* data) override;
        std::expected<std::unique_ptr<RHI::Texture>, std::string> CreateTexture(RHI::Format format, RHI::Extent extent,
                                                                                std::span<const std::uint8_t> pixels,
                                                                                bool mipmaps) override;
        std::expected<std::unique_ptr<RHI::Texture>, std::string> CreateTextureArray(RHI::Format format, RHI::Extent extent,
                                                                                    std::uint32_t layers,
                                                                                    std::span<const std::uint8_t> pixels,
                                                                                    bool mipmaps) override;
        std::expected<std::unique_ptr<RHI::Texture>, std::string> CreateCubeTexture(RHI::Format format, std::uint32_t size,
                                                                                    std::span<const std::uint8_t> pixels,
                                                                                    bool mipmaps) override;
        std::expected<std::unique_ptr<RHI::Texture>, std::string> CreateRenderTarget(RHI::Format format, RHI::Extent extent,
                                                                                     bool sampled) override;
        std::expected<std::unique_ptr<RHI::Texture>, std::string> CreateCubeRenderTarget(RHI::Format format,
                                                                                            std::uint32_t size) override;
        std::expected<std::unique_ptr<RHI::Sampler>, std::string> CreateSampler(const RHI::SamplerDesc& desc) override;
        std::expected<std::unique_ptr<RHI::BindGroupLayout>, std::string> CreateBindGroupLayout(const RHI::BindGroupLayoutDesc& desc) override;
        std::unique_ptr<RHI::BindGroup> CreateBindGroup(const RHI::BindGroupLayout& layout) override;
        std::expected<std::unique_ptr<RHI::Pipeline>, std::string> CreatePipeline(const RHI::PipelineDesc& desc) override;
        std::expected<std::unique_ptr<RHI::Pipeline>, std::string> CreateComputePipeline(const RHI::ComputePipelineDesc& desc) override;
        bool Submit(const std::function<void(RHI::CommandList&)>& record) override;

        bool ResizeSwapchain(RHI::Extent extent, bool vsync) override;
        RHI::Extent GetSwapchainExtent() const override;
        RHI::Format GetSwapchainFormat() const override;
        RHI::PresentMode GetPresentMode() const override;

        RHI::FrameStatus BeginFrame() override;
        RHI::CommandList& GetCommandList() override { return m_commandList; }
        std::uint32_t GetFrameIndex() const override { return m_frameIndex; }
        bool EndFrame() override;
        std::span<const double> GetTimestamps() const override { return {m_timestamps.data(), m_timestampCount}; }
        const RHI::WaitTimes& GetWaitTimes() const override { return m_waitTimes; }

        private:
        struct Frame
        {
            VkCommandPool commandPool = VK_NULL_HANDLE;
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            VkSemaphore imageAvailable = VK_NULL_HANDLE;
            VkFence inFlight = VK_NULL_HANDLE;
            VkQueryPool queries = VK_NULL_HANDLE;  // RHI::MaxTimestamps heures ; VK_NULL_HANDLE sans heures du GPU
            std::uint32_t writtenTimestamps = 0;   // cases écrites la dernière fois que l'image a servi
            // Étapes de la dernière image soumise dans ces ressources (BeginStep), pour le diagnostic de plantage.
            std::array<const char*, RHI::MaxSteps> steps{};
            std::uint32_t stepCount = 0;
            std::uint32_t serial = 0; // numéro de cette image, écrit par le GPU au début et à la fin de chaque étape
            bool submitted = false;
        };

        // Relit les heures de l'image dont le GPU vient de finir (après l'attente de sa barrière).
        void ReadTimestamps(const Frame& frame);

        // Appel Vulkan de l'image : sur la perte du device (GPU bloqué, pilote relancé), écrit le diagnostic.
        bool CheckFrame(VkResult result, const char* call);
        void ReportDeviceLost();

        RhiDevice() = default;
        bool CreateFrames();
        bool RecreateSwapchain();

        std::unique_ptr<Vk::Device> m_device;
        std::unique_ptr<Swapchain> m_swapchain;
        std::array<Frame, RHI::FramesInFlight> m_frames{};
        std::uint32_t m_frameIndex = 0;
        std::uint32_t m_imageIndex = 0;
        RHI::Extent m_extent;
        bool m_vsync = true;
        RhiCommandList m_commandList;
        double m_timestampPeriod = 0.0; // nanosecondes par tic ; 0 : pas d'heures du GPU
        std::array<double, RHI::MaxTimestamps> m_timestamps{};
        std::size_t m_timestampCount = 0;
        RHI::WaitTimes m_waitTimes;
        Vk::Buffer m_stepMarkers; // FramesInFlight x MaxSteps x (début, fin) ; vide sans VK_AMD_buffer_marker
        std::uint32_t m_frameSerial = 0;
        bool m_lostReported = false;
    };
}
