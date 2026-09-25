#include "Flux/Renderer/Renderer.hpp"

#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Platform/Window.hpp"
#include "RHI/Vulkan/VulkanBuffer.hpp"
#include "RHI/Vulkan/VulkanDevice.hpp"
#include "RHI/Vulkan/VulkanPipeline.hpp"
#include "RHI/Vulkan/VulkanSwapchain.hpp"

#include <array>
#include <cstddef>
#include <cstring>

namespace Flux {
    namespace {
        // Deux frames en vol : le CPU prépare la suivante pendant que le GPU dessine la courante.
        constexpr uint32_t FramesInFlight = 2;

        // Doit correspondre à VertexInput dans shaders/Triangle.slang.
        struct TriangleVertex {
            float position[2];
            float color[3];
        };

        // Repère Vulkan : y vers le bas, donc le premier sommet est en haut.
        constexpr std::array<TriangleVertex, 3> TriangleVertices{{
            {{0.0f, -0.5f}, {1.0f, 0.0f, 0.0f}},
            {{0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}},
            {{-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}},
        }};

        struct FrameData {
            VkCommandPool commandPool = VK_NULL_HANDLE;
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            VkSemaphore imageAvailable = VK_NULL_HANDLE;
            VkFence inFlight = VK_NULL_HANDLE;
        };

        void TransitionImage(VkCommandBuffer commandBuffer, VkImage image,
                             VkImageLayout from, VkImageLayout to,
                             VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = srcStage;
            barrier.srcAccessMask = srcAccess;
            barrier.dstStageMask = dstStage;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = from;
            barrier.newLayout = to;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
        }
    }

    struct Renderer::Impl {
        Window& window;
        std::unique_ptr<Vk::Device> device;
        std::unique_ptr<Vk::Swapchain> swapchain;
        std::array<FrameData, FramesInFlight> frames{};
        uint32_t frameIndex = 0;
        // Déclarés après device : détruits avant lui.
        Vk::Buffer triangleVertexBuffer;
        Vk::GraphicsPipeline trianglePipeline;
        Math::LinearColor clearColor{0.02f, 0.02f, 0.05f, 1.0f};

        explicit Impl(Window& w) : window(w) {}

        ~Impl() {
            if (!device) {
                return;
            }
            device->WaitIdle();
            for (FrameData& frame : frames) {
                vkDestroyFence(device->GetHandle(), frame.inFlight, nullptr);
                vkDestroySemaphore(device->GetHandle(), frame.imageAvailable, nullptr);
                vkDestroyCommandPool(device->GetHandle(), frame.commandPool, nullptr);
            }
            swapchain.reset();
        }

        bool CreateFrames() {
            for (FrameData& frame : frames) {
                VkCommandPoolCreateInfo poolInfo{};
                poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
                poolInfo.queueFamilyIndex = device->GetGraphicsQueueFamily();
                if (!Vk::Check(vkCreateCommandPool(device->GetHandle(), &poolInfo, nullptr, &frame.commandPool), "vkCreateCommandPool")) {
                    return false;
                }

                VkCommandBufferAllocateInfo allocateInfo{};
                allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocateInfo.commandPool = frame.commandPool;
                allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocateInfo.commandBufferCount = 1;
                if (!Vk::Check(vkAllocateCommandBuffers(device->GetHandle(), &allocateInfo, &frame.commandBuffer), "vkAllocateCommandBuffers")) {
                    return false;
                }

                VkSemaphoreCreateInfo semaphoreInfo{};
                semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                if (!Vk::Check(vkCreateSemaphore(device->GetHandle(), &semaphoreInfo, nullptr, &frame.imageAvailable), "vkCreateSemaphore")) {
                    return false;
                }

                // Signalée dès le départ pour que la première attente ne bloque pas.
                VkFenceCreateInfo fenceInfo{};
                fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
                if (!Vk::Check(vkCreateFence(device->GetHandle(), &fenceInfo, nullptr, &frame.inFlight), "vkCreateFence")) {
                    return false;
                }
            }
            return true;
        }

        std::expected<void, std::string> CreateTriangle() {
            auto buffer = Vk::Buffer::Create(device->GetAllocator(), sizeof(TriangleVertices),
                                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, Vk::Buffer::Memory::Upload);
            if (!buffer) {
                return std::unexpected(buffer.error());
            }
            std::memcpy(buffer->GetMappedData(), TriangleVertices.data(), sizeof(TriangleVertices));
            triangleVertexBuffer = std::move(*buffer);

            const VkVertexInputBindingDescription binding{0, sizeof(TriangleVertex), VK_VERTEX_INPUT_RATE_VERTEX};
            const std::array attributes{
                VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TriangleVertex, position)},
                VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TriangleVertex, color)},
            };

            Vk::GraphicsPipelineDesc desc;
            desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Triangle.spv";
            desc.vertexBindings = {&binding, 1};
            desc.vertexAttributes = attributes;
            desc.colorFormat = swapchain->GetFormat();

            auto pipeline = Vk::GraphicsPipeline::Create(device->GetHandle(), desc);
            if (!pipeline) {
                return std::unexpected(pipeline.error());
            }
            trianglePipeline = std::move(*pipeline);
            return {};
        }

        VkExtent2D WindowExtent() const {
            const Math::Vector2i size = window.GetSize();
            return {static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y)};
        }

        bool RecreateSwapchain() {
            device->WaitIdle();
            return swapchain->Recreate(WindowExtent());
        }

        void DrawTriangle(VkCommandBuffer commandBuffer) const {
            const VkExtent2D extent = swapchain->GetExtent();
            const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
            const VkRect2D scissor{{0, 0}, extent};
            const VkBuffer vertexBuffer = triangleVertexBuffer.GetHandle();
            const VkDeviceSize offset = 0;

            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, trianglePipeline.GetHandle());
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &offset);
            vkCmdDraw(commandBuffer, static_cast<uint32_t>(TriangleVertices.size()), 1, 0, 0);
        }

        void Record(VkCommandBuffer commandBuffer, uint32_t imageIndex) const {
            const VkImage image = swapchain->GetImage(imageIndex);

            // Le contenu précédent est jeté : on efface tout de suite après.
            TransitionImage(commandBuffer, image,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = swapchain->GetImageView(imageIndex);
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue.color = {{clearColor.r, clearColor.g, clearColor.b, clearColor.a}};

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = {{0, 0}, swapchain->GetExtent()};
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;

            vkCmdBeginRendering(commandBuffer, &renderingInfo);
            DrawTriangle(commandBuffer);
            vkCmdEndRendering(commandBuffer);

            TransitionImage(commandBuffer, image,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
        }

        bool RenderFrame() {
            const VkExtent2D extent = WindowExtent();
            if (extent.width == 0 || extent.height == 0) {
                return true; // fenêtre réduite : rien à présenter
            }
            const VkExtent2D current = swapchain->GetExtent();
            if (extent.width != current.width || extent.height != current.height) {
                if (!RecreateSwapchain()) {
                    return false;
                }
            }

            FrameData& frame = frames[frameIndex];
            const VkDevice vkDevice = device->GetHandle();

            if (!Vk::Check(vkWaitForFences(vkDevice, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences")) {
                return false;
            }

            uint32_t imageIndex = 0;
            const VkResult acquire = vkAcquireNextImageKHR(vkDevice, swapchain->GetHandle(), UINT64_MAX,
                                                           frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
            if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
                return RecreateSwapchain(); // on retentera à la frame suivante
            }
            if (!Vk::Check(acquire, "vkAcquireNextImageKHR")) {
                return false;
            }

            // Réinitialisée seulement une fois sûr de soumettre, sinon la frame suivante attendrait indéfiniment.
            vkResetFences(vkDevice, 1, &frame.inFlight);
            vkResetCommandPool(vkDevice, frame.commandPool, 0);

            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
            Record(frame.commandBuffer, imageIndex);
            if (!Vk::Check(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer")) {
                return false;
            }

            const VkSemaphore renderFinished = swapchain->GetRenderFinishedSemaphore(imageIndex);

            VkSemaphoreSubmitInfo waitInfo{};
            waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            waitInfo.semaphore = frame.imageAvailable;
            waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

            VkSemaphoreSubmitInfo signalInfo{};
            signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            signalInfo.semaphore = renderFinished;
            signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

            VkCommandBufferSubmitInfo commandInfo{};
            commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            commandInfo.commandBuffer = frame.commandBuffer;

            VkSubmitInfo2 submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            submitInfo.waitSemaphoreInfoCount = 1;
            submitInfo.pWaitSemaphoreInfos = &waitInfo;
            submitInfo.commandBufferInfoCount = 1;
            submitInfo.pCommandBufferInfos = &commandInfo;
            submitInfo.signalSemaphoreInfoCount = 1;
            submitInfo.pSignalSemaphoreInfos = &signalInfo;
            if (!Vk::Check(vkQueueSubmit2(device->GetGraphicsQueue(), 1, &submitInfo, frame.inFlight), "vkQueueSubmit2")) {
                return false;
            }

            const VkSwapchainKHR swapchainHandle = swapchain->GetHandle();
            VkPresentInfoKHR presentInfo{};
            presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            presentInfo.waitSemaphoreCount = 1;
            presentInfo.pWaitSemaphores = &renderFinished;
            presentInfo.swapchainCount = 1;
            presentInfo.pSwapchains = &swapchainHandle;
            presentInfo.pImageIndices = &imageIndex;
            const VkResult present = vkQueuePresentKHR(device->GetPresentQueue(), &presentInfo);

            frameIndex = (frameIndex + 1) % FramesInFlight;

            if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR) {
                return RecreateSwapchain();
            }
            return Vk::Check(present, "vkQueuePresentKHR");
        }
    };

    std::expected<std::unique_ptr<Renderer>, std::string> Renderer::Create(Window& window) {
        auto impl = std::make_unique<Impl>(window);

        auto device = Vk::Device::Create(window.GetNativeHandle());
        if (!device) {
            return std::unexpected(device.error());
        }
        impl->device = std::move(device.value());

        impl->swapchain = std::make_unique<Vk::Swapchain>(*impl->device);
        if (!impl->swapchain->Recreate(impl->WindowExtent())) {
            return std::unexpected("création de la swapchain");
        }
        if (!impl->CreateFrames()) {
            return std::unexpected("création des ressources de frame");
        }
        if (auto triangle = impl->CreateTriangle(); !triangle) {
            return std::unexpected(triangle.error());
        }

        return std::unique_ptr<Renderer>(new Renderer(std::move(impl)));
    }

    Renderer::Renderer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

    Renderer::~Renderer() = default;

    bool Renderer::RenderFrame() {
        return m_impl->RenderFrame();
    }

    void Renderer::SetClearColor(Math::LinearColor color) {
        m_impl->clearColor = color;
    }
}
