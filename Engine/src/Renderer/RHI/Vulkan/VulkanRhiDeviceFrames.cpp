#include "Renderer/RHI/Vulkan/VulkanRhiDevice.hpp"

#include "Renderer/RHI/Vulkan/VulkanRhiBuffer.hpp"
#include "Renderer/RHI/Vulkan/VulkanTypes.hpp"

#include <array>
#include <chrono>
#include <utility>

// RhiDevice, partie images : soumissions, swapchain, envois asynchrones et libérations différées. La création des
// ressources est dans VulkanRhiDevice.cpp.
namespace Flux::Vk {
    void RhiDevice::ReleaseStaging(std::unique_ptr<RHI::StagingBuffer> staging) {
        // Sans fenêtre, les copies sont déjà finies (envois attendus) : rendue tout de suite.
        if (staging && m_swapchain) {
            m_device->GetTransfer().KeepUntilCopied(static_cast<RhiStagingBuffer&>(*staging).Take());
        }
    }

    void RhiDevice::BeginAsyncUploads() {
        // Sans fenêtre, chaque envoi est attendu tout de suite : rien à différer.
        m_asyncUploads = m_swapchain != nullptr;
        if (m_asyncUploads) {
            m_device->GetTransfer().BeginAsync();
        }
    }

    std::uint64_t RhiDevice::EndAsyncUploads() {
        if (!std::exchange(m_asyncUploads, false)) {
            return 0;
        }
        return m_device->GetTransfer().EndAsync();
    }

    bool RhiDevice::IsUploadReady(std::uint64_t ticket) const {
        return m_device->GetTransfer().IsReady(ticket);
    }

    void RhiDevice::Retire(std::move_only_function<void()> release) {
        if (!m_swapchain) {
            // Sans fenêtre, chaque soumission est attendue : le GPU ne se sert plus de rien.
            release();
            return;
        }
        m_retired.push_back({m_device->GetTransfer().GetMarks(), std::move(release)});
    }

    void RhiDevice::RunRetired(bool all) {
        const Vk::Transfer& transfer = m_device->GetTransfer();
        // Dans l'ordre : les marques ne font que croître. Retirée avant l'appel, qui peut en ajouter.
        while (!m_retired.empty() && (all || transfer.IsDone(m_retired.front().marks))) {
            std::move_only_function<void()> release = std::move(m_retired.front().release);
            m_retired.pop_front();
            release();
        }
    }

    void RhiDevice::WaitIdle() {
        m_device->WaitIdle();
        RunRetired(true);
    }

    bool RhiDevice::Submit(const std::function<void(RHI::CommandList&)>& record) {
        const bool submitted = m_device->ImmediateSubmit([&](VkCommandBuffer commandBuffer) {
            RhiCommandList commandList;
            commandList.SetMeshShaders(m_device->HasMeshShaders());
            commandList.Begin(commandBuffer, VK_NULL_HANDLE, VK_NULL_HANDLE, {}, VK_NULL_HANDLE);
            record(commandList);
        });
        RunRetired(false);
        return submitted;
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

    RHI::PresentMode RhiDevice::GetPresentMode() const {
        if (!m_swapchain) {
            return RHI::PresentMode::Other;
        }
        return FromVulkan(m_swapchain->GetPresentMode());
    }

    RHI::FrameStatus RhiDevice::BeginFrame() {
        if (!m_swapchain) {
            return RHI::FrameStatus::Error; // sans fenêtre : Submit seulement
        }
        using Clock = std::chrono::steady_clock;
        const auto milliseconds = [](Clock::duration duration) { return std::chrono::duration<double, std::milli>(duration).count(); };
        const Clock::time_point waitStart = Clock::now();
        if (!m_crash->Check(m_frames->Wait(m_frameIndex), "vkWaitForFences")) {
            return RHI::FrameStatus::Error;
        }
        const Clock::time_point acquireStart = Clock::now();
        m_waitTimes.frame = milliseconds(acquireStart - waitStart);
        m_timestamps->Read(m_frameIndex, m_writtenTimestamps[m_frameIndex]);
        m_device->GetTransfer().Collect();
        RunRetired(false);

        const Frames::Frame& frame = m_frames->Get(m_frameIndex);
        const VkResult acquire = m_swapchain->Acquire(frame.imageAvailable, m_imageIndex);
        m_waitTimes.acquire = milliseconds(Clock::now() - acquireStart);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            return RecreateSwapchain() ? RHI::FrameStatus::Skip : RHI::FrameStatus::Error; // on retentera à l'image suivante
        }
        if (!m_crash->Check(acquire, "vkAcquireNextImageKHR")) {
            return RHI::FrameStatus::Error;
        }

        m_frames->Begin(m_frameIndex);
        m_commandList.Begin(frame.commandBuffer, m_swapchain->GetImage(m_imageIndex), m_swapchain->GetImageView(m_imageIndex),
                            m_swapchain->GetExtent(), m_timestamps->GetPool(m_frameIndex));
        m_crash->BeginFrame(m_frameIndex, m_commandList);
        return RHI::FrameStatus::Ready;
    }

    bool RhiDevice::EndFrame() {
        const Frames::Frame& frame = m_frames->Get(m_frameIndex);
        m_commandList.EndStep();
        m_commandList.TransitionToPresent();
        m_writtenTimestamps[m_frameIndex] = m_commandList.GetWrittenTimestamps();
        if (!Check(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer")) {
            return false;
        }

        VkSemaphoreSubmitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waitInfo.semaphore = frame.imageAvailable;
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        // Les envois enregistrés pendant la préparation de l'image d'abord : elle dessine peut-être ce qu'ils copient.
        Vk::Transfer& transfer = m_device->GetTransfer();
        Vk::Transfer::GraphicsPart uploads = transfer.TakeGraphics();
        std::array<VkSemaphoreSubmitInfo, 2> waitInfos{waitInfo, transfer.GetWait(uploads)};

        std::array<VkSemaphoreSubmitInfo, 2> signalInfos{};
        signalInfos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalInfos[0].semaphore = m_swapchain->GetRenderFinishedSemaphore(m_imageIndex);
        signalInfos[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signalInfos[1] = transfer.GetSignal();

        m_commandInfos.clear();
        for (VkCommandBuffer commands : uploads.commands) {
            m_commandInfos.push_back({VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, commands, 0});
        }
        m_commandInfos.push_back({VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, frame.commandBuffer, 0});

        VkSubmitInfo2 submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submitInfo.waitSemaphoreInfoCount = uploads.transferValue > 0 ? 2 : 1;
        submitInfo.pWaitSemaphoreInfos = waitInfos.data();
        submitInfo.commandBufferInfoCount = static_cast<std::uint32_t>(m_commandInfos.size());
        submitInfo.pCommandBufferInfos = m_commandInfos.data();
        submitInfo.signalSemaphoreInfoCount = static_cast<std::uint32_t>(signalInfos.size());
        submitInfo.pSignalSemaphoreInfos = signalInfos.data();
        const bool submitted = m_crash->Check(vkQueueSubmit2(m_device->GetGraphicsQueue(), 1, &submitInfo, frame.inFlight),
                                              "vkQueueSubmit2");
        transfer.GraphicsSubmitted(uploads, submitted);
        if (!submitted) {
            return false;
        }
        m_crash->Submitted(m_frameIndex);

        const auto presentStart = std::chrono::steady_clock::now();
        const VkResult present = m_swapchain->Present(m_device->GetPresentQueue(), m_imageIndex);
        m_waitTimes.present = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentStart).count();

        m_frameIndex = (m_frameIndex + 1) % RHI::FramesInFlight;

        if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR) {
            return RecreateSwapchain();
        }
        return m_crash->Check(present, "vkQueuePresentKHR");
    }
}
