#pragma once

// RHI::Device sur Vulkan : crée les ressources et assemble les blocs du backend (device, swapchain, images en vol,
// heures du GPU, diagnostic de plantage, envois).

#include "Flux/Renderer/RHI/Device.hpp"
#include "Renderer/RHI/Vulkan/VulkanCommandBatch.hpp"
#include "Renderer/RHI/Vulkan/VulkanCrashMarkers.hpp"
#include "Renderer/RHI/Vulkan/VulkanDevice.hpp"
#include "Renderer/RHI/Vulkan/VulkanFrames.hpp"
#include "Renderer/RHI/Vulkan/VulkanRhiCommandList.hpp"
#include "Renderer/RHI/Vulkan/VulkanSwapchain.hpp"
#include "Renderer/RHI/Vulkan/VulkanTimestamps.hpp"
#include "Renderer/RHI/Vulkan/VulkanTransfer.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Flux::Vk {
    class RhiDevice final : public RHI::Device {
        public:
        // nativeWindowHandle nul : sans fenêtre ni swapchain (Submit seulement).
        static std::expected<std::unique_ptr<RhiDevice>, std::string> Create(void* nativeWindowHandle, RHI::Extent extent, bool vsync);
        ~RhiDevice() override;

        std::string_view GetName() const override { return m_device->GetName(); }
        std::string_view GetApiVersion() const override { return m_device->GetApiVersion(); }
        bool HasMeshShaders() const override { return m_device->HasMeshShaders(); }
        std::uint32_t GetMeshPreferredThreads() const override { return m_device->GetMeshPreferredThreads(); }
        bool HasTextureCompressionBC() const override { return m_device->HasTextureCompressionBC(); }
        bool HasRayTracing() const override { return m_device->HasRayTracing(); }
        RHI::MemoryUsage GetMemoryUsage() const override;
        void WaitIdle() override;

        std::expected<std::unique_ptr<RHI::Buffer>, std::string> CreateBuffer(const RHI::BufferDesc& desc, const void* data) override;
        bool WriteBuffer(RHI::Buffer& buffer, std::uint64_t offset, const void* data, std::uint64_t size, bool inUse) override;
        std::expected<std::unique_ptr<RHI::Texture>, std::string> CreateTexture(RHI::Format format, RHI::Extent extent,
                                                                                std::span<const std::uint8_t> pixels,
                                                                                bool mipmaps) override;
        std::expected<std::unique_ptr<RHI::Texture>, std::string> CreateTexture(RHI::Format format, RHI::Extent extent,
                                                                                const RHI::StagingBuffer& source,
                                                                                std::uint64_t offset, bool mipmaps) override;
        std::expected<std::unique_ptr<RHI::StagingBuffer>, std::string> CreateStagingBuffer(std::uint64_t size) override;
        void ReleaseStaging(std::unique_ptr<RHI::StagingBuffer> staging) override;
        void BeginAsyncUploads() override;
        std::uint64_t EndAsyncUploads() override;
        bool IsUploadReady(std::uint64_t ticket) const override;
        void Retire(std::move_only_function<void()> release) override;
        std::expected<std::unique_ptr<RHI::Texture>, std::string> CreateTextureArray(RHI::Format format, RHI::Extent extent,
                                                                                    std::uint32_t layers,
                                                                                    std::span<const std::uint8_t> pixels,
                                                                                    bool mipmaps) override;
        std::expected<std::unique_ptr<RHI::Texture>, std::string> CreateTextureArrayLevels(RHI::Format format, RHI::Extent extent,
                                                                                         std::uint32_t layers, std::uint32_t levels,
                                                                                         std::span<const std::uint8_t> pixels) override;
        std::expected<std::unique_ptr<RHI::Texture>, std::string> CreateCubeTexture(RHI::Format format, std::uint32_t size,
                                                                                    std::span<const std::uint8_t> pixels,
                                                                                    bool mipmaps) override;
        std::expected<std::unique_ptr<RHI::Texture>, std::string> CreateVolumeTexture(RHI::Format format, RHI::Extent extent,
                                                                                      std::uint32_t depth, bool mipmaps) override;
        std::expected<std::unique_ptr<RHI::Texture>, std::string> CreateRenderTarget(RHI::Format format, RHI::Extent extent,
                                                                                     bool sampled, bool storage,
                                                                                     std::uint32_t mipLevels) override;
        std::expected<std::unique_ptr<RHI::Texture>, std::string> CreateCubeRenderTarget(RHI::Format format,
                                                                                            std::uint32_t size,
                                                                                            std::uint32_t mipLevels) override;
        std::expected<std::unique_ptr<RHI::AccelerationStructure>, std::string>
        CreateBottomLevel(std::span<const RHI::AccelerationGeometry> geometries) override;
        std::expected<std::unique_ptr<RHI::AccelerationStructure>, std::string> CreateTopLevel(std::uint32_t maxInstances) override;
        std::expected<std::unique_ptr<RHI::Sampler>, std::string> CreateSampler(const RHI::SamplerDesc& desc) override;
        std::expected<std::unique_ptr<RHI::BindGroupLayout>, std::string> CreateBindGroupLayout(const RHI::BindGroupLayoutDesc& desc) override;
        std::unique_ptr<RHI::BindGroup> CreateBindGroup(const RHI::BindGroupLayout& layout) override;
        std::expected<std::unique_ptr<RHI::Pipeline>, std::string> CreatePipeline(const RHI::PipelineDesc& desc) override;
        std::expected<std::unique_ptr<RHI::Pipeline>, std::string> CreateComputePipeline(const RHI::ComputePipelineDesc& desc) override;
        std::expected<std::unique_ptr<RHI::Upscaler>, std::string> CreateUpscaler(RHI::Extent outputSize) override;
        bool Submit(const std::function<void(RHI::CommandList&)>& record) override;

        bool ResizeSwapchain(RHI::Extent extent, bool vsync) override;
        RHI::Extent GetSwapchainExtent() const override;
        RHI::Format GetSwapchainFormat() const override;
        RHI::PresentMode GetPresentMode() const override;

        RHI::FrameStatus BeginFrame() override;
        RHI::CommandList& GetCommandList() override { return m_commandList; }
        std::uint32_t GetFrameIndex() const override { return m_frameIndex; }
        bool EndFrame() override;
        std::span<const double> GetTimestamps() const override;
        const RHI::WaitTimes& GetWaitTimes() const override { return m_waitTimes; }
        bool RequestCapture() override;
        std::optional<ImageData> TakeCapture() override;

        private:
        // Appelle les libérations différées dont le GPU a fini (toutes : après une attente complète).
        void RunRetired(bool all);

        RhiDevice() = default;
        bool RecreateSwapchain();
        // Relit la capture copiée par l'image qui vient de finir dans ces ressources (BeginFrame, après l'attente).
        void ReadCapture();

        // Dans cet ordre : tout ce qui suit est détruit avant le device.
        std::unique_ptr<Vk::Device> m_device;
        std::unique_ptr<Swapchain> m_swapchain; // nul sans fenêtre
        std::unique_ptr<Frames> m_frames;       // de même
        std::array<std::unique_ptr<CommandBatch>, RHI::FramesInFlight> m_batches; // command buffers de chaque image
        std::unique_ptr<CommandBatch> m_submitBatch;                             // ceux de Submit
        std::unique_ptr<Timestamps> m_timestamps;
        std::unique_ptr<CrashMarkers> m_crash;
        std::array<std::uint32_t, RHI::FramesInFlight> m_writtenTimestamps{}; // cases écrites par chaque image
        std::uint32_t m_frameIndex = 0;
        std::uint32_t m_imageIndex = 0;
        RHI::Extent m_extent;
        bool m_vsync = true;
        RhiCommandList m_commandList;
        RhiCommandList m_submitList;
        RHI::WaitTimes m_waitTimes;
        struct Retired
        {
            Vk::Transfer::Marks marks;
            std::move_only_function<void()> release;
        };
        std::deque<Retired> m_retired;
        bool m_asyncUploads = false; // entre BeginAsyncUploads et EndAsyncUploads, avec une fenêtre
        std::vector<VkCommandBufferSubmitInfo> m_commandInfos; // de la soumission de l'image, réutilisé
        // Capture d'écran : demandée, copiée par l'image frameIndex (relue quand elle a fini), puis prête.
        bool m_captureRequested = false;
        struct PendingCapture
        {
            Vk::Buffer buffer;
            std::uint32_t frameIndex;
            VkExtent2D extent;
            VkFormat format;
        };
        std::optional<PendingCapture> m_pendingCapture;
        std::optional<ImageData> m_capture;
    };
}
