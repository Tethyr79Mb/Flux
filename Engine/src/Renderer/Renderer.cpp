#include "Flux/Renderer/Renderer.hpp"

#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Platform/Window.hpp"
#include "Flux/UI/DrawList.hpp"
#include "Flux/UI/Font.hpp"
#include "RHI/Vulkan/VulkanBuffer.hpp"
#include "RHI/Vulkan/VulkanDevice.hpp"
#include "RHI/Vulkan/VulkanImage.hpp"
#include "RHI/Vulkan/VulkanPipeline.hpp"
#include "RHI/Vulkan/VulkanSwapchain.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <span>

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

        // Doit correspondre à OverlayParams dans shaders/Overlay.slang.
        struct OverlayParams {
            float screenSize[2];
            float atlasSize[2];
            float distanceRange;
            float padding;
        };

        // Taille minimale d'un vertex buffer d'overlay : évite de réallouer pour quelques caractères de plus.
        constexpr VkDeviceSize MinOverlayBufferSize = 64 * 1024;

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

        // Overlay 2D (FluxUI) : atlas MSDF + un vertex buffer par frame en vol.
        std::unique_ptr<UI::Font> font;
        std::unique_ptr<UI::DrawList> overlay;
        Vk::Image atlasImage;
        VkSampler atlasSampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout overlaySetLayout = VK_NULL_HANDLE;
        VkDescriptorPool overlayDescriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet overlaySet = VK_NULL_HANDLE;
        Vk::GraphicsPipeline overlayPipeline;
        std::array<Vk::Buffer, FramesInFlight> overlayBuffers;
        uint32_t overlayVertexCount = 0;

        explicit Impl(Window& w) : window(w) {}

        ~Impl() {
            if (!device) {
                return;
            }
            device->WaitIdle();
            const VkDevice vkDevice = device->GetHandle();
            vkDestroyDescriptorPool(vkDevice, overlayDescriptorPool, nullptr); // libère aussi overlaySet
            vkDestroyDescriptorSetLayout(vkDevice, overlaySetLayout, nullptr);
            vkDestroySampler(vkDevice, atlasSampler, nullptr);
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
            buffer->Write(TriangleVertices.data(), sizeof(TriangleVertices));
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

        std::expected<void, std::string> CreateOverlay() {
            const std::filesystem::path fontPath = Platform::GetExecutableDirectory() / "assets" / "fonts" / "Arimo.ttf";
            auto loadedFont = UI::Font::Load(fontPath);
            if (!loadedFont) {
                return std::unexpected(loadedFont.error());
            }
            font = std::move(*loadedFont);
            overlay = std::make_unique<UI::DrawList>(*font);

            // UNORM et pas SRGB : l'atlas contient des distances, pas des couleurs.
            const Math::Vector2i atlasSize = font->GetAtlasSize();
            auto image = Vk::Image::CreateTexture(*device, VK_FORMAT_R8G8B8A8_UNORM,
                                                  {static_cast<uint32_t>(atlasSize.x), static_cast<uint32_t>(atlasSize.y)},
                                                  font->GetAtlasPixels());
            if (!image) {
                return std::unexpected(image.error());
            }
            atlasImage = std::move(*image);

            const VkDevice vkDevice = device->GetHandle();

            // Filtrage linéaire obligatoire : le MSDF reconstruit le contour entre les texels.
            VkSamplerCreateInfo samplerInfo{};
            samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            if (!Vk::Check(vkCreateSampler(vkDevice, &samplerInfo, nullptr, &atlasSampler), "vkCreateSampler")) {
                return std::unexpected("création du sampler de l'atlas");
            }

            const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
            setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            setLayoutInfo.bindingCount = 1;
            setLayoutInfo.pBindings = &binding;
            if (!Vk::Check(vkCreateDescriptorSetLayout(vkDevice, &setLayoutInfo, nullptr, &overlaySetLayout), "vkCreateDescriptorSetLayout")) {
                return std::unexpected("création du descriptor set layout de l'overlay");
            }

            const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.maxSets = 1;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &poolSize;
            if (!Vk::Check(vkCreateDescriptorPool(vkDevice, &poolInfo, nullptr, &overlayDescriptorPool), "vkCreateDescriptorPool")) {
                return std::unexpected("création du descriptor pool de l'overlay");
            }

            VkDescriptorSetAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocateInfo.descriptorPool = overlayDescriptorPool;
            allocateInfo.descriptorSetCount = 1;
            allocateInfo.pSetLayouts = &overlaySetLayout;
            if (!Vk::Check(vkAllocateDescriptorSets(vkDevice, &allocateInfo, &overlaySet), "vkAllocateDescriptorSets")) {
                return std::unexpected("allocation du descriptor set de l'overlay");
            }

            const VkDescriptorImageInfo imageInfo{atlasSampler, atlasImage.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = overlaySet;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfo;
            vkUpdateDescriptorSets(vkDevice, 1, &write, 0, nullptr);

            const VkVertexInputBindingDescription vertexBinding{0, sizeof(UI::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
            const std::array attributes{
                VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UI::Vertex, position)},
                VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UI::Vertex, uv)},
                VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UI::Vertex, color)},
            };
            const VkPushConstantRange pushConstants{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(OverlayParams)};

            Vk::GraphicsPipelineDesc desc;
            desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Overlay.spv";
            desc.vertexBindings = {&vertexBinding, 1};
            desc.vertexAttributes = attributes;
            desc.setLayouts = {&overlaySetLayout, 1};
            desc.pushConstants = {&pushConstants, 1};
            desc.colorFormat = swapchain->GetFormat();
            desc.alphaBlend = true;

            auto pipeline = Vk::GraphicsPipeline::Create(vkDevice, desc);
            if (!pipeline) {
                return std::unexpected(pipeline.error());
            }
            overlayPipeline = std::move(*pipeline);
            return {};
        }

        // Appelé après l'attente de la fence : le GPU n'utilise plus le buffer de cette frame.
        bool UploadOverlay() {
            const std::span<const UI::Vertex> vertices = overlay->GetVertices();
            overlayVertexCount = static_cast<uint32_t>(vertices.size());
            if (vertices.empty()) {
                return true;
            }

            const VkDeviceSize bytes = vertices.size_bytes();
            Vk::Buffer& buffer = overlayBuffers[frameIndex];
            if (buffer.GetSize() < bytes) {
                auto grown = Vk::Buffer::Create(device->GetAllocator(), std::bit_ceil(std::max(bytes, MinOverlayBufferSize)),
                                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, Vk::Buffer::Memory::Upload);
                if (!grown) {
                    std::fprintf(stderr, "Flux : %s\n", grown.error().c_str());
                    return false;
                }
                buffer = std::move(*grown);
            }
            buffer.Write(vertices.data(), bytes);
            return true;
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

        void DrawOverlay(VkCommandBuffer commandBuffer) const {
            if (overlayVertexCount == 0) {
                return;
            }
            const VkExtent2D extent = swapchain->GetExtent();
            const Math::Vector2i atlasSize = font->GetAtlasSize();
            const OverlayParams params{
                {static_cast<float>(extent.width), static_cast<float>(extent.height)},
                {static_cast<float>(atlasSize.x), static_cast<float>(atlasSize.y)},
                font->GetDistanceRange(),
                0.0f,
            };
            const VkBuffer vertexBuffer = overlayBuffers[frameIndex].GetHandle();
            const VkDeviceSize offset = 0;

            // Viewport et scissor déjà réglés par DrawTriangle, pour toute la passe.
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline.GetHandle());
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline.GetLayout(),
                                    0, 1, &overlaySet, 0, nullptr);
            vkCmdPushConstants(commandBuffer, overlayPipeline.GetLayout(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &offset);
            vkCmdDraw(commandBuffer, overlayVertexCount, 1, 0, 0);
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
            DrawOverlay(commandBuffer);
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
            if (!UploadOverlay()) {
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
        if (auto overlay = impl->CreateOverlay(); !overlay) {
            return std::unexpected(overlay.error());
        }

        return std::unique_ptr<Renderer>(new Renderer(std::move(impl)));
    }

    Renderer::Renderer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

    Renderer::~Renderer() = default;

    bool Renderer::RenderFrame() {
        const bool ok = m_impl->RenderFrame();
        m_impl->overlay->Clear(); // même si la frame n'a pas été présentée (fenêtre réduite, swapchain recréée)
        return ok;
    }

    void Renderer::SetClearColor(Math::LinearColor color) {
        m_impl->clearColor = color;
    }

    UI::DrawList& Renderer::GetOverlay() {
        return *m_impl->overlay;
    }

    std::string_view Renderer::GetDeviceName() const {
        return m_impl->device->GetName();
    }
}
