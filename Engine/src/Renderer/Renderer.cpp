#include "Flux/Renderer/Renderer.hpp"

#include "Flux/Animation/Animation.hpp"
#include "Flux/Assets/Image.hpp"
#include "Flux/Assets/Model.hpp"
#include "Flux/Graphics/Camera.hpp"
#include "Flux/Graphics/Font.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Platform/Window.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/UI/DrawList.hpp"
#include "Renderer/Vulkan/VulkanBuffer.hpp"
#include "Renderer/Vulkan/VulkanDevice.hpp"
#include "Renderer/Vulkan/VulkanImage.hpp"
#include "Renderer/Vulkan/VulkanPipeline.hpp"
#include "Renderer/Vulkan/VulkanSwapchain.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Flux {
    namespace {
        // Deux frames en vol : le CPU prépare la suivante pendant que le GPU dessine la courante.
        constexpr uint32_t FramesInFlight = 2;

        // Profondeur 32 bits flottante : obligatoire pour que le reverse-Z tienne l'échelle galaxie.
        constexpr VkFormat DepthFormat = VK_FORMAT_D32_SFLOAT;

        static_assert(sizeof(ModelVertex) == 32, "ModelVertex doit correspondre à VertexInput dans shaders/Mesh.slang");
        static_assert(sizeof(ModelSkinVertex) == 24, "ModelSkinVertex doit correspondre à SkinnedVertexInput dans shaders/Mesh.slang");

        // Doit correspondre à MeshParams dans shaders/Mesh.slang : 128 octets, le minimum garanti des push constants.
        struct MeshParams {
            Math::Matrix4f modelViewProjection;
            // normalColumns[0][3] : seuil d'alpha ; normalColumns[1][3] : premier os (bits d'un entier), modèle animé
            std::array<std::array<float, 4>, 3> normalColumns;
            Math::LinearColor color;
        };
        static_assert(sizeof(MeshParams) == 128);

        // Cube unité centré ; chaque face a ses 4 sommets (normale propre, image entière).
        ModelData BuildCube() {
            struct Face {
                Math::Vector3f normal;
                Math::Vector3f tangent;
            };
            constexpr std::array<Face, 6> faces{{
                {{1, 0, 0}, {0, 0, -1}},
                {{-1, 0, 0}, {0, 0, 1}},
                {{0, 1, 0}, {1, 0, 0}},
                {{0, -1, 0}, {1, 0, 0}},
                {{0, 0, 1}, {1, 0, 0}},
                {{0, 0, -1}, {-1, 0, 0}},
            }};
            ModelData mesh;
            for (const Face& face : faces) {
                // bitangent = normal x tangent, donc tangent x bitangent = normal : sens antihoraire vu de l'extérieur.
                const Math::Vector3f bitangent = Math::Cross(face.normal, face.tangent);
                const Math::Vector3f center = face.normal * 0.5f;
                const Math::Vector3f u = face.tangent * 0.5f;
                const Math::Vector3f v = bitangent * 0.5f;
                const auto base = static_cast<uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back({center - u - v, face.normal, {0.0f, 1.0f}});
                mesh.vertices.push_back({center + u - v, face.normal, {1.0f, 1.0f}});
                mesh.vertices.push_back({center + u + v, face.normal, {1.0f, 0.0f}});
                mesh.vertices.push_back({center - u + v, face.normal, {0.0f, 0.0f}});
                for (const uint32_t corner : {0u, 1u, 2u, 0u, 2u, 3u}) {
                    mesh.indices.push_back(base + corner);
                }
            }
            mesh.primitives.push_back({0, static_cast<uint32_t>(mesh.indices.size()), 0});
            mesh.materials.emplace_back();
            return mesh;
        }

        // Doit correspondre à OverlayParams dans shaders/Overlay.slang.
        struct OverlayParams {
            float screenSize[2];
            float atlasSize[2];
            float distanceRange;
            float mode; // 0 : atlas MSDF ; 1 : image
        };

        // Images chargées au plus : un descriptor set chacune, réservés d'avance.
        constexpr uint32_t MaxTextures = 256;
        // Intervalle entre deux vérifications de la date d'un fichier d'image (rechargé s'il a changé) :
        // assez court pour voir une retouche aussitôt, assez long pour ne pas interroger le disque à chaque frame.
        constexpr std::chrono::milliseconds TextureCheckInterval{1000};

        // Image de l'interface chargée depuis un fichier, et de quoi voir si le fichier a changé.
        struct Texture {
            Vk::Image image;
            UI::TextureId id = UI::FontTexture; // FontTexture : pas (encore) chargée
            Math::Vector2i size;
            std::filesystem::file_time_type writeTime{};
            std::chrono::steady_clock::time_point checked{};
        };

        // Matériaux de modèles chargés au plus : un descriptor set chacun (son image de couleur de base).
        constexpr uint32_t MaxMaterials = 4096;

        struct GpuMaterial {
            Math::LinearColor baseColor{1.0f, 1.0f, 1.0f, 1.0f};
            float alphaCutoff = 0.0f;
            bool doubleSided = false;
            VkDescriptorSet set = VK_NULL_HANDLE; // VK_NULL_HANDLE : celui de l'image blanche
        };

        // Matrices d'os envoyées au GPU par frame : de quoi animer des dizaines de personnages sans réallouer.
        constexpr VkDeviceSize MinJointBufferSize = 64 * 1024;

        // Modèle 3D envoyé au GPU (le cube du moteur, ou un fichier glTF), et de quoi voir si le fichier a changé.
        struct GpuModel {
            Vk::Buffer vertices;
            Vk::Buffer indices; // 32 bits
            Vk::Buffer skin;    // ModelSkinVertex, modèle animé seulement
            ModelSkeleton skeleton; // gardé sur le CPU : les poses y sont calculées à chaque frame
            std::vector<Vk::Image> images;
            std::vector<GpuMaterial> materials;
            std::vector<ModelPrimitive> primitives;
            Math::Box3f bounds;
            bool loaded = false;
            std::filesystem::file_time_type writeTime{};
            std::chrono::steady_clock::time_point checked{};
        };

        // Une entité à dessiner : son modèle et ses matrices, calculées pour la frame.
        struct MeshDraw {
            const GpuModel* model;
            Math::Matrix4f modelViewProjection;
            std::array<std::array<float, 4>, 3> normalColumns;
            Math::LinearColor tint;
            uint32_t firstJoint = 0; // modèle animé : ses os dans le buffer des os de la frame
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
                             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
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
            barrier.subresourceRange = {aspect, 0, 1, 0, 1};

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
        Vk::Image depthImage;
        // Maillages : faces arrière éliminées, ou gardées (matériaux double face). Même layout pour les deux.
        Vk::GraphicsPipeline meshPipeline;
        Vk::GraphicsPipeline meshPipelineDoubleSided;
        // Modèles animés : un second flux de sommets (os et poids), et les matrices des os en set 1.
        Vk::GraphicsPipeline skinnedPipeline;
        Vk::GraphicsPipeline skinnedPipelineDoubleSided;
        VkDescriptorSetLayout jointSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool jointDescriptorPool = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, FramesInFlight> jointSets{};
        std::array<Vk::Buffer, FramesInFlight> jointBuffers;
        std::vector<Math::Matrix4f> jointMatrices; // de la frame en préparation
        PoseEvaluator poseEvaluator;
        VkSampler meshSampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool materialDescriptorPool = VK_NULL_HANDLE;
        Vk::Image whiteImage; // 1 x 1 : matériaux sans image
        VkDescriptorSet whiteSet = VK_NULL_HANDLE;
        GpuModel cube;
        // Modèles des fichiers, par chemin (UTF-8, normalisé) ; les échecs aussi, pour ne pas relire à chaque frame.
        std::unordered_map<std::string, GpuModel> models;
        std::filesystem::path assetsDirectory;
        // Préparés sur le CPU avant l'enregistrement des commandes, réutilisés d'une frame à l'autre.
        std::vector<RelativeTransform> relativeTransforms;
        std::vector<MeshDraw> meshDraws;
        Math::LinearColor clearColor{0.02f, 0.02f, 0.05f, 1.0f};
        bool vsync = true;
        bool vsyncChanged = false;
        Math::Vector2f sceneViewportMin; // en pixels ; vide : toute la fenêtre
        Math::Vector2f sceneViewportMax;

        // Overlay 2D (FluxUI) : atlas MSDF + un vertex buffer par frame en vol.
        std::unique_ptr<UI::Font> font;
        std::unique_ptr<UI::DrawList> overlay;
        Vk::Image atlasImage;
        VkSampler atlasSampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout overlaySetLayout = VK_NULL_HANDLE;
        VkDescriptorPool overlayDescriptorPool = VK_NULL_HANDLE;
        // Un set par texture, indexé par UI::TextureId : [0] est l'atlas de la police.
        std::vector<VkDescriptorSet> overlaySets;
        // Images de l'interface, par chemin (UTF-8, normalisé).
        std::unordered_map<std::string, Texture> textures;
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
            vkDestroyDescriptorPool(vkDevice, jointDescriptorPool, nullptr);
            vkDestroyDescriptorSetLayout(vkDevice, jointSetLayout, nullptr);
            vkDestroyDescriptorPool(vkDevice, materialDescriptorPool, nullptr); // libère aussi les sets des matériaux
            vkDestroyDescriptorSetLayout(vkDevice, materialSetLayout, nullptr);
            vkDestroySampler(vkDevice, meshSampler, nullptr);
            vkDestroyDescriptorPool(vkDevice, overlayDescriptorPool, nullptr); // libère aussi overlaySets
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

        std::expected<void, std::string> CreateDepth() {
            depthImage = {}; // libère l'ancienne avant d'allouer la nouvelle
            auto depth = Vk::Image::CreateAttachment(*device, DepthFormat, swapchain->GetExtent(),
                                                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            if (!depth) {
                return std::unexpected(depth.error());
            }
            depthImage = std::move(*depth);
            return {};
        }

        std::expected<void, std::string> CreateMeshes() {
            const VkDevice vkDevice = device->GetHandle();

            // Répété (les UV des modèles sortent souvent de [0, 1]), filtré entre les mips.
            VkSamplerCreateInfo samplerInfo{};
            samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
            if (!Vk::Check(vkCreateSampler(vkDevice, &samplerInfo, nullptr, &meshSampler), "vkCreateSampler")) {
                return std::unexpected("création du sampler des maillages");
            }

            const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
            setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            setLayoutInfo.bindingCount = 1;
            setLayoutInfo.pBindings = &binding;
            if (!Vk::Check(vkCreateDescriptorSetLayout(vkDevice, &setLayoutInfo, nullptr, &materialSetLayout), "vkCreateDescriptorSetLayout")) {
                return std::unexpected("création du descriptor set layout des matériaux");
            }

            // Sets libérables un par un : un modèle rechargé rend ceux de l'ancienne version.
            const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 + MaxMaterials};
            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            poolInfo.maxSets = 1 + MaxMaterials;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &poolSize;
            if (!Vk::Check(vkCreateDescriptorPool(vkDevice, &poolInfo, nullptr, &materialDescriptorPool), "vkCreateDescriptorPool")) {
                return std::unexpected("création du descriptor pool des matériaux");
            }

            constexpr std::array<uint8_t, 4> white{255, 255, 255, 255};
            auto image = Vk::Image::CreateTexture(*device, VK_FORMAT_R8G8B8A8_SRGB, {1, 1}, white);
            if (!image) {
                return std::unexpected(image.error());
            }
            whiteImage = std::move(*image);
            whiteSet = AllocateMaterialSet(whiteImage);
            if (whiteSet == VK_NULL_HANDLE) {
                return std::unexpected("allocation du descriptor set de l'image blanche");
            }

            const VkVertexInputBindingDescription vertexBinding{0, sizeof(ModelVertex), VK_VERTEX_INPUT_RATE_VERTEX};
            const std::array attributes{
                VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ModelVertex, position)},
                VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ModelVertex, normal)},
                VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ModelVertex, uv)},
            };
            const VkPushConstantRange pushConstants{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshParams)};

            Vk::GraphicsPipelineDesc desc;
            desc.shaderPath = Platform::GetExecutableDirectory() / "shaders" / "Mesh.spv";
            desc.vertexBindings = {&vertexBinding, 1};
            desc.vertexAttributes = attributes;
            desc.setLayouts = {&materialSetLayout, 1};
            desc.pushConstants = {&pushConstants, 1};
            desc.colorFormat = swapchain->GetFormat();
            desc.depthFormat = DepthFormat;
            desc.depthTest = true;
            desc.depthWrite = true;
            desc.cullMode = VK_CULL_MODE_BACK_BIT;
            auto pipeline = Vk::GraphicsPipeline::Create(vkDevice, desc);
            if (!pipeline) {
                return std::unexpected(pipeline.error());
            }
            meshPipeline = std::move(*pipeline);

            desc.cullMode = VK_CULL_MODE_NONE;
            auto doubleSided = Vk::GraphicsPipeline::Create(vkDevice, desc);
            if (!doubleSided) {
                return std::unexpected(doubleSided.error());
            }
            meshPipelineDoubleSided = std::move(*doubleSided);

            if (auto skinned = CreateSkinnedPipelines(desc, vertexBinding, attributes); !skinned) {
                return skinned;
            }

            auto uploaded = UploadModel(BuildCube(), "cube");
            if (!uploaded) {
                return std::unexpected(uploaded.error());
            }
            cube = std::move(*uploaded);
            return {};
        }

        // Même description que les maillages statiques, plus les os et poids (binding 1) et les matrices des os
        // (set 1, lues par le vertex shader).
        std::expected<void, std::string> CreateSkinnedPipelines(Vk::GraphicsPipelineDesc desc,
                                                                const VkVertexInputBindingDescription& vertexBinding,
                                                                std::span<const VkVertexInputAttributeDescription> modelAttributes) {
            const VkDevice vkDevice = device->GetHandle();
            const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
            setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            setLayoutInfo.bindingCount = 1;
            setLayoutInfo.pBindings = &binding;
            if (!Vk::Check(vkCreateDescriptorSetLayout(vkDevice, &setLayoutInfo, nullptr, &jointSetLayout), "vkCreateDescriptorSetLayout")) {
                return std::unexpected("création du descriptor set layout des os");
            }
            const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, FramesInFlight};
            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.maxSets = FramesInFlight;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &poolSize;
            if (!Vk::Check(vkCreateDescriptorPool(vkDevice, &poolInfo, nullptr, &jointDescriptorPool), "vkCreateDescriptorPool")) {
                return std::unexpected("création du descriptor pool des os");
            }
            for (uint32_t f = 0; f < FramesInFlight; ++f) {
                VkDescriptorSetAllocateInfo allocateInfo{};
                allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                allocateInfo.descriptorPool = jointDescriptorPool;
                allocateInfo.descriptorSetCount = 1;
                allocateInfo.pSetLayouts = &jointSetLayout;
                if (!Vk::Check(vkAllocateDescriptorSets(vkDevice, &allocateInfo, &jointSets[f]), "vkAllocateDescriptorSets")) {
                    return std::unexpected("allocation des descriptor sets des os");
                }
                if (!GrowJointBuffer(f, MinJointBufferSize)) {
                    return std::unexpected("création du buffer des os");
                }
            }

            const std::array bindings{
                vertexBinding,
                VkVertexInputBindingDescription{1, sizeof(ModelSkinVertex), VK_VERTEX_INPUT_RATE_VERTEX},
            };
            std::vector<VkVertexInputAttributeDescription> attributes(modelAttributes.begin(), modelAttributes.end());
            attributes.push_back({3, 1, VK_FORMAT_R16G16B16A16_UINT, offsetof(ModelSkinVertex, joints)});
            attributes.push_back({4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ModelSkinVertex, weights)});
            const std::array setLayouts{materialSetLayout, jointSetLayout};
            desc.vertexEntryPoint = "vertexSkinned";
            desc.vertexBindings = bindings;
            desc.vertexAttributes = attributes;
            desc.setLayouts = setLayouts;
            desc.cullMode = VK_CULL_MODE_BACK_BIT;
            auto pipeline = Vk::GraphicsPipeline::Create(vkDevice, desc);
            if (!pipeline) {
                return std::unexpected(pipeline.error());
            }
            skinnedPipeline = std::move(*pipeline);
            desc.cullMode = VK_CULL_MODE_NONE;
            auto doubleSided = Vk::GraphicsPipeline::Create(vkDevice, desc);
            if (!doubleSided) {
                return std::unexpected(doubleSided.error());
            }
            skinnedPipelineDoubleSided = std::move(*doubleSided);
            return {};
        }

        // Buffer des os de la frame f agrandi à size octets au moins, et son descriptor set mis à jour. Appelé quand
        // le GPU n'utilise plus cette frame (après l'attente de sa fence).
        bool GrowJointBuffer(uint32_t f, VkDeviceSize size) {
            Vk::Buffer& buffer = jointBuffers[f];
            if (buffer.GetSize() >= size) {
                return true;
            }
            auto grown = Vk::Buffer::Create(device->GetAllocator(), std::bit_ceil(std::max(size, MinJointBufferSize)),
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Vk::Buffer::Memory::Upload);
            if (!grown) {
                std::fprintf(stderr, "Flux : %s\n", grown.error().c_str());
                return false;
            }
            buffer = std::move(*grown);
            const VkDescriptorBufferInfo bufferInfo{buffer.GetHandle(), 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = jointSets[f];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &bufferInfo;
            vkUpdateDescriptorSets(device->GetHandle(), 1, &write, 0, nullptr);
            return true;
        }

        VkDescriptorSet AllocateMaterialSet(const Vk::Image& image) const {
            VkDescriptorSetAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocateInfo.descriptorPool = materialDescriptorPool;
            allocateInfo.descriptorSetCount = 1;
            allocateInfo.pSetLayouts = &materialSetLayout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            // Pool épuisé : pas une erreur fatale, le matériau prendra l'image blanche.
            if (vkAllocateDescriptorSets(device->GetHandle(), &allocateInfo, &set) != VK_SUCCESS) {
                return VK_NULL_HANDLE;
            }
            const VkDescriptorImageInfo imageInfo{meshSampler, image.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfo;
            vkUpdateDescriptorSets(device->GetHandle(), 1, &write, 0, nullptr);
            return set;
        }

        // Buffer en mémoire vidéo, rempli par une copie depuis un buffer intermédiaire (attend la fin de la copie).
        std::expected<Vk::Buffer, std::string> CreateStaticBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage) const {
            auto staging = Vk::Buffer::Create(device->GetAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Vk::Buffer::Memory::Upload);
            if (!staging) {
                return std::unexpected(staging.error());
            }
            staging->Write(data, size);
            auto buffer = Vk::Buffer::Create(device->GetAllocator(), size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             Vk::Buffer::Memory::GpuOnly);
            if (!buffer) {
                return std::unexpected(buffer.error());
            }
            const bool copied = device->ImmediateSubmit([&](VkCommandBuffer commandBuffer) {
                const VkBufferCopy region{0, 0, size};
                vkCmdCopyBuffer(commandBuffer, staging->GetHandle(), buffer->GetHandle(), 1, &region);
            });
            if (!copied) {
                return std::unexpected("envoi d'un maillage au GPU");
            }
            return buffer;
        }

        // Sommets, indices, images et matériaux envoyés au GPU. Une image refusée par le GPU laisse son matériau blanc.
        std::expected<GpuModel, std::string> UploadModel(const ModelData& data, std::string_view name) {
            GpuModel model;
            auto vertices = CreateStaticBuffer(data.vertices.data(), data.vertices.size() * sizeof(ModelVertex),
                                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            if (!vertices) {
                return std::unexpected(vertices.error());
            }
            model.vertices = std::move(*vertices);
            auto indices = CreateStaticBuffer(data.indices.data(), data.indices.size() * sizeof(uint32_t),
                                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            if (!indices) {
                return std::unexpected(indices.error());
            }
            model.indices = std::move(*indices);
            if (data.IsAnimated()) {
                auto skin = CreateStaticBuffer(data.skinVertices.data(), data.skinVertices.size() * sizeof(ModelSkinVertex),
                                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                if (!skin) {
                    return std::unexpected(skin.error());
                }
                model.skin = std::move(*skin);
                model.skeleton = data.skeleton;
            }

            std::vector<int> imageSlots(data.images.size(), -1); // indice dans model.images
            for (std::size_t i = 0; i < data.images.size(); ++i) {
                const ImageData& pixels = data.images[i];
                auto image = Vk::Image::CreateTexture(*device, VK_FORMAT_R8G8B8A8_SRGB,
                                                      {static_cast<uint32_t>(pixels.size.x), static_cast<uint32_t>(pixels.size.y)},
                                                      pixels.rgba, true);
                if (!image) {
                    std::fprintf(stderr, "Flux : modèle %.*s : image %zu : %s\n", static_cast<int>(name.size()), name.data(), i,
                                 image.error().c_str());
                    continue;
                }
                imageSlots[i] = static_cast<int>(model.images.size());
                model.images.push_back(std::move(*image));
            }
            for (const ModelMaterial& source : data.materials) {
                GpuMaterial material;
                material.baseColor = source.baseColor;
                material.alphaCutoff = source.alphaCutoff;
                material.doubleSided = source.doubleSided;
                if (source.baseColorImage >= 0 && imageSlots[static_cast<std::size_t>(source.baseColorImage)] >= 0) {
                    material.set = AllocateMaterialSet(model.images[static_cast<std::size_t>(imageSlots[static_cast<std::size_t>(source.baseColorImage)])]);
                    if (material.set == VK_NULL_HANDLE) {
                        std::fprintf(stderr, "Flux : plus de %u matériaux chargés, « %s » sans image\n", MaxMaterials, source.name.c_str());
                    }
                }
                model.materials.push_back(material);
            }
            model.primitives = data.primitives;
            model.bounds = data.bounds;
            model.loaded = true;
            return model;
        }

        void FreeMaterialSets(GpuModel& model) const {
            for (GpuMaterial& material : model.materials) {
                if (material.set != VK_NULL_HANDLE) {
                    vkFreeDescriptorSets(device->GetHandle(), materialDescriptorPool, 1, &material.set);
                    material.set = VK_NULL_HANDLE;
                }
            }
        }

        // Lit le fichier et remplace le modèle ; en cas d'échec, l'ancienne version (s'il y en a une) reste.
        void ReloadModel(const std::filesystem::path& path, GpuModel& model) {
            const std::string name = Platform::ToUtf8(path.filename());
            auto data = LoadModelFile(path);
            if (!data) {
                std::fprintf(stderr, "Flux : modèle %s\n", data.error().c_str());
                return;
            }
            for (const std::string& warning : data->warnings) {
                std::fprintf(stderr, "Flux : modèle %s : %s\n", name.c_str(), warning.c_str());
            }
            // Rechargement (fichier retouché) : les frames en vol dessinent peut-être encore l'ancienne version.
            // Rare : attendre le GPU suffit, et les envois ci-dessous l'attendent de toute façon.
            device->WaitIdle();
            auto uploaded = UploadModel(*data, name);
            if (!uploaded) {
                std::fprintf(stderr, "Flux : modèle %s : %s\n", name.c_str(), uploaded.error().c_str());
                return;
            }
            FreeMaterialSets(model);
            const std::filesystem::file_time_type writeTime = model.writeTime;
            const std::chrono::steady_clock::time_point checked = model.checked;
            model = std::move(*uploaded);
            model.writeTime = writeTime;
            model.checked = checked;
        }

        // Modèle du fichier, chargé au premier appel puis relu s'il change ; nullptr s'il est illisible.
        const GpuModel* FindModel(const std::filesystem::path& path) {
            const auto now = std::chrono::steady_clock::now();
            auto [found, created] = models.try_emplace(Platform::ToGenericUtf8(path.lexically_normal()));
            GpuModel& model = found->second;
            if (created || now - model.checked >= TextureCheckInterval) {
                model.checked = now;
                std::error_code error;
                const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, error);
                // Fichier absent : essayé une fois (pour le signaler), puis de nouveau quand il apparaît.
                if (created || (!error && writeTime != model.writeTime)) {
                    model.writeTime = error ? std::filesystem::file_time_type{} : writeTime;
                    ReloadModel(path, model);
                }
            }
            return model.loaded ? &model : nullptr;
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

            // L'atlas, puis les images.
            const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 + MaxTextures};
            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.maxSets = 1 + MaxTextures;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &poolSize;
            if (!Vk::Check(vkCreateDescriptorPool(vkDevice, &poolInfo, nullptr, &overlayDescriptorPool), "vkCreateDescriptorPool")) {
                return std::unexpected("création du descriptor pool de l'overlay");
            }

            const VkDescriptorSet atlasSet = AllocateOverlaySet();
            if (atlasSet == VK_NULL_HANDLE) {
                return std::unexpected("allocation du descriptor set de l'overlay");
            }
            WriteOverlaySet(atlasSet, atlasImage);
            overlaySets.push_back(atlasSet); // UI::FontTexture

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
            desc.depthFormat = DepthFormat; // l'overlay passe par-dessus tout : pas de test de profondeur
            desc.alphaBlend = true;

            auto pipeline = Vk::GraphicsPipeline::Create(vkDevice, desc);
            if (!pipeline) {
                return std::unexpected(pipeline.error());
            }
            overlayPipeline = std::move(*pipeline);
            return {};
        }

        VkDescriptorSet AllocateOverlaySet() const {
            VkDescriptorSetAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocateInfo.descriptorPool = overlayDescriptorPool;
            allocateInfo.descriptorSetCount = 1;
            allocateInfo.pSetLayouts = &overlaySetLayout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (!Vk::Check(vkAllocateDescriptorSets(device->GetHandle(), &allocateInfo, &set), "vkAllocateDescriptorSets")) {
                return VK_NULL_HANDLE;
            }
            return set;
        }

        // Le sampler de l'atlas (linéaire, bords prolongés) convient aussi aux images.
        void WriteOverlaySet(VkDescriptorSet set, const Vk::Image& image) const {
            const VkDescriptorImageInfo imageInfo{atlasSampler, image.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfo;
            vkUpdateDescriptorSets(device->GetHandle(), 1, &write, 0, nullptr);
        }

        // Lit le fichier et remplace l'image de la texture ; en cas d'échec, l'ancienne image (s'il y en a une) reste.
        void ReloadTexture(const std::filesystem::path& path, Texture& texture) {
            auto pixels = LoadImageFile(path);
            if (!pixels) {
                std::fprintf(stderr, "Flux : image %s\n", pixels.error().c_str());
                return;
            }
            // SRGB : les couleurs d'une image sont en sRGB, le GPU les ramène en linéaire à la lecture.
            auto image = Vk::Image::CreateTexture(*device, VK_FORMAT_R8G8B8A8_SRGB,
                                                  {static_cast<uint32_t>(pixels->size.x), static_cast<uint32_t>(pixels->size.y)},
                                                  pixels->rgba);
            if (!image) {
                std::fprintf(stderr, "Flux : image %s : %s\n", Platform::ToUtf8(path).c_str(), image.error().c_str());
                return;
            }
            if (texture.id == UI::FontTexture) {
                if (overlaySets.size() > MaxTextures) {
                    std::fprintf(stderr, "Flux : plus de %u images chargées, %s ignorée\n", MaxTextures,
                                 Platform::ToUtf8(path).c_str());
                    return;
                }
                const VkDescriptorSet set = AllocateOverlaySet();
                if (set == VK_NULL_HANDLE) {
                    return;
                }
                texture.id = static_cast<UI::TextureId>(overlaySets.size());
                overlaySets.push_back(set);
            } else {
                // Rechargement (fichier retouché) : les frames en vol dessinent peut-être encore l'ancienne image,
                // et un descriptor set ne se modifie pas pendant qu'il sert. Rare : attendre le GPU suffit.
                device->WaitIdle();
            }
            texture.image = std::move(*image);
            texture.size = pixels->size;
            WriteOverlaySet(overlaySets[texture.id], texture.image);
        }

        std::optional<UI::TextureInfo> LoadTexture(const std::filesystem::path& path) {
            const auto now = std::chrono::steady_clock::now();
            auto [found, created] = textures.try_emplace(Platform::ToGenericUtf8(path.lexically_normal()));
            Texture& texture = found->second;
            if (created || now - texture.checked >= TextureCheckInterval) {
                texture.checked = now;
                std::error_code error;
                const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, error);
                // Fichier absent : essayé une fois (pour le signaler), puis de nouveau quand il apparaît.
                if (created || (!error && writeTime != texture.writeTime)) {
                    texture.writeTime = error ? std::filesystem::file_time_type{} : writeTime;
                    ReloadTexture(path, texture);
                }
            }
            if (texture.id == UI::FontTexture) {
                return std::nullopt;
            }
            return UI::TextureInfo{texture.id, texture.size};
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
            if (!swapchain->Recreate(WindowExtent(), vsync)) {
                return false;
            }
            if (auto depth = CreateDepth(); !depth) {
                std::fprintf(stderr, "Flux : %s\n", depth.error().c_str());
                return false;
            }
            return true;
        }

        // Rectangle de la scène (en pixels entiers), borné à la fenêtre : la vue 3D de l'éditeur, ou toute la fenêtre.
        VkRect2D SceneRect() const {
            const VkExtent2D extent = swapchain->GetExtent();
            const auto clampTo = [](float value, uint32_t limit) {
                return static_cast<int32_t>(std::clamp(std::round(value), 0.0f, static_cast<float>(limit)));
            };
            const int32_t x0 = clampTo(sceneViewportMin.x, extent.width);
            const int32_t y0 = clampTo(sceneViewportMin.y, extent.height);
            const int32_t x1 = clampTo(sceneViewportMax.x, extent.width);
            const int32_t y1 = clampTo(sceneViewportMax.y, extent.height);
            if (x1 - x0 < 1 || y1 - y0 < 1) {
                return {{0, 0}, extent};
            }
            return {{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
        }

        void DrawScene(VkCommandBuffer commandBuffer) const {
            const VkRect2D scissor = SceneRect();
            const VkViewport viewport{static_cast<float>(scissor.offset.x), static_cast<float>(scissor.offset.y),
                                      static_cast<float>(scissor.extent.width), static_cast<float>(scissor.extent.height),
                                      0.0f, 1.0f};
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

            // Changements d'état seulement quand il le faut : les entités d'un même modèle se suivent souvent.
            const Vk::GraphicsPipeline* boundPipeline = nullptr;
            const GpuModel* boundModel = nullptr;
            VkDescriptorSet boundSet = VK_NULL_HANDLE;
            bool jointsBound = false;
            for (const MeshDraw& draw : meshDraws) {
                const GpuModel& model = *draw.model;
                const bool animated = model.skin.GetHandle() != VK_NULL_HANDLE;
                if (&model != boundModel) {
                    boundModel = &model;
                    const std::array vertexBuffers{model.vertices.GetHandle(), model.skin.GetHandle()};
                    const std::array<VkDeviceSize, 2> offsets{};
                    vkCmdBindVertexBuffers(commandBuffer, 0, animated ? 2 : 1, vertexBuffers.data(), offsets.data());
                    vkCmdBindIndexBuffer(commandBuffer, model.indices.GetHandle(), 0, VK_INDEX_TYPE_UINT32);
                }
                for (const ModelPrimitive& primitive : model.primitives) {
                    const GpuMaterial& material = model.materials[primitive.material];
                    const Vk::GraphicsPipeline& pipeline =
                        animated ? (material.doubleSided ? skinnedPipelineDoubleSided : skinnedPipeline)
                                 : (material.doubleSided ? meshPipelineDoubleSided : meshPipeline);
                    if (&pipeline != boundPipeline) {
                        // Layout différent (statique / animé) : les sets liés ne sont plus garantis, on les relie.
                        if (boundPipeline && boundPipeline->GetLayout() != pipeline.GetLayout()) {
                            boundSet = VK_NULL_HANDLE;
                            jointsBound = false;
                        }
                        boundPipeline = &pipeline;
                        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.GetHandle());
                    }
                    if (animated && !jointsBound) {
                        jointsBound = true;
                        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.GetLayout(), 1, 1,
                                                &jointSets[frameIndex], 0, nullptr);
                    }
                    const VkDescriptorSet set = material.set != VK_NULL_HANDLE ? material.set : whiteSet;
                    if (set != boundSet) {
                        boundSet = set;
                        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.GetLayout(), 0, 1, &set, 0, nullptr);
                    }
                    MeshParams params{draw.modelViewProjection, draw.normalColumns,
                                      {material.baseColor.r * draw.tint.r, material.baseColor.g * draw.tint.g,
                                       material.baseColor.b * draw.tint.b, material.baseColor.a * draw.tint.a}};
                    params.normalColumns[0][3] = material.alphaCutoff;
                    params.normalColumns[1][3] = std::bit_cast<float>(draw.firstJoint);
                    vkCmdPushConstants(commandBuffer, pipeline.GetLayout(),
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);
                    vkCmdDrawIndexed(commandBuffer, primitive.indexCount, 1, primitive.firstIndex, 0, 0);
                }
            }
        }

        // Une matrice par maillage visible, calculée relativement à la caméra : la soustraction des positions
        // se fait en double (secteurs compris), le GPU ne voit que de petits nombres.
        void PrepareScene(const Scene& scene, const Camera& camera) {
            const VkExtent2D extent = SceneRect().extent;
            const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
            const Math::Matrix4f viewProjection = camera.GetProjectionMatrix(aspect) * camera.GetViewMatrix();

            scene.ComputeRelativeTransforms(camera.position, relativeTransforms);
            meshDraws.clear();
            jointMatrices.clear();
            for (const RelativeTransform& transform : relativeTransforms) {
                const GpuModel* model = nullptr;
                Math::LinearColor tint;
                if (const auto* renderer = scene.TryGet<MeshRendererComponent>(transform.entity)) {
                    model = &cube;
                    tint = renderer->color;
                } else if (const auto* component = scene.TryGet<ModelComponent>(transform.entity);
                           component && !component->model.empty() && !assetsDirectory.empty()) {
                    model = FindModel(assetsDirectory / Platform::PathFromUtf8(component->model));
                    tint = component->tint;
                }
                if (!model) {
                    continue;
                }
                const Math::Matrix4f world = Math::Matrix4f::Translation(Math::Vector3f(transform.position))
                                           * transform.rotation.ToMatrix()
                                           * Math::Matrix4f::Scale(transform.scale);
                // Normales : rotation x inverse de l'échelle (une échelle non uniforme penche les normales).
                // Échelle nulle (objet aplati) : un grand nombre plutôt qu'une division par zéro.
                const auto inverse = [](float scale) { return std::abs(scale) > 1.0e-12f ? 1.0f / scale : 1.0e12f; };
                const Math::Vector3f x = transform.rotation.Rotate(Math::Vector3f{inverse(transform.scale.x), 0.0f, 0.0f});
                const Math::Vector3f y = transform.rotation.Rotate(Math::Vector3f{0.0f, inverse(transform.scale.y), 0.0f});
                const Math::Vector3f z = transform.rotation.Rotate(Math::Vector3f{0.0f, 0.0f, inverse(transform.scale.z)});
                const std::array<std::array<float, 4>, 3> normalColumns{{{x.x, x.y, x.z, 0.0f},
                                                                         {y.x, y.y, y.z, 0.0f},
                                                                         {z.x, z.y, z.z, 0.0f}}};
                uint32_t firstJoint = 0;
                if (!model->skeleton.joints.empty()) {
                    firstJoint = static_cast<uint32_t>(jointMatrices.size());
                    Pose(*model, scene.TryGet<AnimatorComponent>(transform.entity));
                    const std::span<const Math::Matrix4f> joints = poseEvaluator.GetJointMatrices();
                    jointMatrices.insert(jointMatrices.end(), joints.begin(), joints.end());
                }
                meshDraws.push_back({model, viewProjection * world, normalColumns, tint, firstJoint});
            }
            UploadJoints();
        }

        // Pose d'un modèle animé, d'après la lecture de son AnimatorComponent ; sans lui, la pose de repos.
        // Avant le jeu (rien de joué encore), la première image de l'animation choisie.
        void Pose(const GpuModel& model, const AnimatorComponent* animator) {
            if (!animator) {
                poseEvaluator.Evaluate(model.skeleton, {});
                return;
            }
            const std::string& playing = animator->playing.empty() ? animator->animation : animator->playing;
            const AnimationSample to{FindAnimation(model.skeleton, playing), animator->time, animator->loop && !animator->playingOnce};
            if (animator->fade >= 1.0f) {
                poseEvaluator.Evaluate(model.skeleton, to);
                return;
            }
            const AnimationSample from{FindAnimation(model.skeleton, animator->previous), animator->previousTime,
                                       animator->loop && !animator->previousOnce};
            poseEvaluator.Evaluate(model.skeleton, from, to, animator->fade);
        }

        // Matrices des os de la frame, par colonnes comme jointColumns dans shaders/Mesh.slang. Si le buffer ne
        // peut grandir (mémoire épuisée), les modèles animés ne sont pas dessinés plutôt que lus hors du buffer.
        void UploadJoints() {
            if (jointMatrices.empty()) {
                return;
            }
            static_assert(sizeof(Math::Matrix4f) == 64);
            const VkDeviceSize bytes = jointMatrices.size() * sizeof(Math::Matrix4f);
            if (GrowJointBuffer(frameIndex, bytes)) {
                jointBuffers[frameIndex].Write(jointMatrices.data(), bytes);
            } else {
                std::erase_if(meshDraws, [](const MeshDraw& draw) { return !draw.model->skeleton.joints.empty(); });
            }
        }

        void DrawOverlay(VkCommandBuffer commandBuffer) const {
            if (overlayVertexCount == 0) {
                return;
            }
            const VkExtent2D extent = swapchain->GetExtent();
            const Math::Vector2i atlasSize = font->GetAtlasSize();
            OverlayParams params{
                {static_cast<float>(extent.width), static_cast<float>(extent.height)},
                {static_cast<float>(atlasSize.x), static_cast<float>(atlasSize.y)},
                font->GetDistanceRange(),
                0.0f,
            };
            const VkBuffer vertexBuffer = overlayBuffers[frameIndex].GetHandle();
            const VkDeviceSize offset = 0;

            // L'interface couvre toute la fenêtre, la scène seulement sa zone.
            const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
            const VkRect2D scissor{{0, 0}, extent};
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline.GetHandle());
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &offset);
            // Un appel par suite de sommets de même texture, dans l'ordre de la liste (le dernier par-dessus).
            UI::TextureId bound = 0;
            bool anyBound = false;
            for (const UI::DrawCommand& command : overlay->GetCommands()) {
                if (command.texture >= overlaySets.size()) {
                    continue; // identifiant inconnu (liste remplie à la main) : ignoré plutôt qu'un plantage GPU
                }
                if (!anyBound || command.texture != bound) {
                    bound = command.texture;
                    anyBound = true;
                    params.mode = bound == UI::FontTexture ? 0.0f : 1.0f;
                    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline.GetLayout(),
                                            0, 1, &overlaySets[bound], 0, nullptr);
                    vkCmdPushConstants(commandBuffer, overlayPipeline.GetLayout(),
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);
                }
                vkCmdDraw(commandBuffer, command.vertexCount, 1, command.firstVertex, 0);
            }
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

            // Deux frames en vol partagent le tampon de profondeur : on attend la fin des écritures de la précédente.
            TransitionImage(commandBuffer, depthImage.GetHandle(),
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT);

            VkRenderingAttachmentInfo depthAttachment{};
            depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAttachment.imageView = depthImage.GetView();
            depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthAttachment.clearValue.depthStencil = {0.0f, 0}; // reverse-Z : 0 = infiniment loin

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = {{0, 0}, swapchain->GetExtent()};
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;
            renderingInfo.pDepthAttachment = &depthAttachment;

            vkCmdBeginRendering(commandBuffer, &renderingInfo);
            DrawScene(commandBuffer);
            DrawOverlay(commandBuffer);
            vkCmdEndRendering(commandBuffer);

            TransitionImage(commandBuffer, image,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
        }

        bool RenderFrame(const Scene& scene, const Camera& camera) {
            const VkExtent2D extent = WindowExtent();
            if (extent.width == 0 || extent.height == 0) {
                return true; // fenêtre réduite : rien à présenter
            }
            const VkExtent2D current = swapchain->GetExtent();
            if (extent.width != current.width || extent.height != current.height || vsyncChanged) {
                vsyncChanged = false;
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
            PrepareScene(scene, camera);
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
        if (!impl->swapchain->Recreate(impl->WindowExtent(), impl->vsync)) {
            return std::unexpected("création de la swapchain");
        }
        if (!impl->CreateFrames()) {
            return std::unexpected("création des ressources de frame");
        }
        if (auto depth = impl->CreateDepth(); !depth) {
            return std::unexpected(depth.error());
        }
        if (auto meshes = impl->CreateMeshes(); !meshes) {
            return std::unexpected(meshes.error());
        }
        if (auto overlay = impl->CreateOverlay(); !overlay) {
            return std::unexpected(overlay.error());
        }

        return std::unique_ptr<Renderer>(new Renderer(std::move(impl)));
    }

    Renderer::Renderer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

    Renderer::~Renderer() = default;

    bool Renderer::RenderFrame(const Scene& scene, const Camera& camera) {
        const bool ok = m_impl->RenderFrame(scene, camera);
        m_impl->overlay->Clear(); // même si la frame n'a pas été présentée (fenêtre réduite, swapchain recréée)
        return ok;
    }

    void Renderer::SetSceneViewport(Math::Vector2f min, Math::Vector2f max) {
        m_impl->sceneViewportMin = min;
        m_impl->sceneViewportMax = max;
    }

    void Renderer::SetClearColor(Math::LinearColor color) {
        m_impl->clearColor = color;
    }

    void Renderer::SetVSync(bool enabled) {
        if (enabled != m_impl->vsync) {
            m_impl->vsync = enabled;
            m_impl->vsyncChanged = true; // swapchain recréée au début de la prochaine frame
        }
    }

    bool Renderer::IsVSyncEnabled() const {
        return m_impl->vsync;
    }

    std::string_view Renderer::GetPresentModeName() const {
        switch (m_impl->swapchain->GetPresentMode()) {
            case VK_PRESENT_MODE_FIFO_KHR: return "FIFO (synchronisé)";
            case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO relaxed";
            case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX (sans déchirure)";
            case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE (déchirure possible)";
            default: return "autre";
        }
    }

    std::optional<UI::TextureInfo> Renderer::LoadTexture(const std::filesystem::path& path) {
        return m_impl->LoadTexture(path);
    }

    void Renderer::SetAssetsDirectory(const std::filesystem::path& directory) {
        if (directory != m_impl->assetsDirectory) {
            m_impl->assetsDirectory = directory;
        }
    }

    std::optional<Math::Box3f> Renderer::LoadModel(const std::filesystem::path& path) {
        if (const GpuModel* model = m_impl->FindModel(path)) {
            return model->bounds;
        }
        return std::nullopt;
    }

    std::vector<std::string> Renderer::GetAnimationNames(const std::filesystem::path& path) {
        std::vector<std::string> names;
        if (const GpuModel* model = m_impl->FindModel(path)) {
            for (const ModelAnimation& animation : model->skeleton.animations) {
                names.push_back(animation.name);
            }
        }
        return names;
    }

    float Renderer::GetAnimationDuration(const std::filesystem::path& path, std::string_view name) {
        const GpuModel* model = m_impl->FindModel(path);
        const ModelAnimation* animation = model ? FindAnimation(model->skeleton, name) : nullptr;
        return animation ? animation->duration : 0.0f;
    }

    UI::DrawList& Renderer::GetOverlay() {
        return *m_impl->overlay;
    }

    std::string_view Renderer::GetDeviceName() const {
        return m_impl->device->GetName();
    }
}
