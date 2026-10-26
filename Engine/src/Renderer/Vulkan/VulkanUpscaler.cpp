#include "Renderer/Vulkan/VulkanUpscaler.hpp"

#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Platform/SharedLibrary.hpp"
#include "Renderer/Vulkan/Vulkan.hpp"
#include "Renderer/Vulkan/VulkanRHI.hpp"

#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>

#include <cfloat>
#include <format>
#include <string>
#include <string_view>

namespace Flux::Vk {
    namespace {
        constexpr const char* LibraryName = "amd_fidelityfx_vk.dll";

        // Messages de la bibliothèque (paramètres refusés, ressources mal décrites) vers le journal.
        void OnMessage(uint32_t type, const wchar_t* message) {
            const std::string text = Platform::ToUtf8(std::filesystem::path(message));
            if (type == FFX_API_MESSAGE_TYPE_ERROR) {
                Log::Error("FSR : {}", text);
            } else {
                Log::Warning("FSR : {}", text);
            }
        }

        void VKAPI_CALL IgnoreCommand() {}

        // Ce que FSR obtient pour chercher ses fonctions Vulkan. Il demande certaines fonctions sous leur ancien nom
        // d'extension (vkGetBufferMemoryRequirements2KHR) : sur un device 1.3 qui n'active pas l'extension, le pilote
        // rend nul, et FSR l'appelait quand même (plantage chez Bruno, 2026-10-25). On rend alors la fonction du cœur
        // de Vulkan, de même signature ; un repère de débogage (vkCmd…EXT) absent devient une commande vide.
        PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
            if (const PFN_vkVoidFunction function = vkGetDeviceProcAddr(device, name)) {
                return function;
            }
            const std::string_view text(name);
            for (const std::string_view suffix : {"KHR", "EXT", "AMD"}) {
                if (text.ends_with(suffix)) {
                    const std::string core(text.substr(0, text.size() - suffix.size()));
                    if (const PFN_vkVoidFunction function = vkGetDeviceProcAddr(device, core.c_str())) {
                        return function;
                    }
                }
            }
            if (text.starts_with("vkCmd")) {
                return IgnoreCommand; // ne rend rien : ses paramètres sont ignorés sans risque
            }
            Log::Warning("FSR : fonction Vulkan {} absente", text);
            return nullptr;
        }

        // Une texture du RHI telle que FSR la décrit : son image Vulkan, sans vue (FSR crée les siennes).
        FfxApiResource Describe(const RHI::Texture& texture, uint32_t state) {
            const auto& vkTexture = static_cast<const RhiTexture&>(texture);
            const RHI::Extent extent = texture.GetExtent();
            FfxApiResource resource{};
            resource.resource = vkTexture.GetImage().GetHandle();
            resource.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
            resource.description.format = ffxApiGetSurfaceFormatVK(ToVulkan(texture.GetFormat()));
            resource.description.width = extent.width;
            resource.description.height = extent.height;
            resource.description.depth = 1;
            resource.description.mipCount = 1;
            resource.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
            resource.description.usage = vkTexture.GetAspect() == VK_IMAGE_ASPECT_DEPTH_BIT ? FFX_API_RESOURCE_USAGE_DEPTHTARGET
                                       : state == FFX_API_RESOURCE_STATE_UNORDERED_ACCESS ? FFX_API_RESOURCE_USAGE_UAV
                                                                                           : FFX_API_RESOURCE_USAGE_READ_ONLY;
            resource.state = state;
            return resource;
        }

        // Fonctions de la DLL (ffx_api.h).
        struct FfxFunctions
        {
            PfnFfxCreateContext createContext = nullptr;
            PfnFfxDestroyContext destroyContext = nullptr;
            PfnFfxQuery query = nullptr;
            PfnFfxDispatch dispatch = nullptr;
        };

        class FsrUpscaler final : public RHI::Upscaler {
            public:
            FsrUpscaler(SharedLibrary library, FfxFunctions functions, RHI::Extent outputSize)
                : m_library(std::move(library)), m_functions(functions), m_outputSize(outputSize) {}
            ~FsrUpscaler() override {
                if (m_context) {
                    m_functions.destroyContext(&m_context, nullptr);
                }
            }
            FsrUpscaler(const FsrUpscaler&) = delete;
            FsrUpscaler& operator=(const FsrUpscaler&) = delete;

            bool Init(const Device& device) {
                ffxCreateBackendVKDesc backend{};
                backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
                backend.vkDevice = device.GetHandle();
                backend.vkPhysicalDevice = device.GetPhysicalDevice();
                backend.vkDeviceProcAddr = GetDeviceProcAddr;
                ffxCreateContextDescUpscale create{};
                create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
                create.header.pNext = &backend.header;
                // Scène HDR linéaire (exposition appliquée après, par la courbe de tons : FSR la mesure lui-même),
                // profondeur reverse-Z infinie.
                create.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED
                             | FFX_UPSCALE_ENABLE_DEPTH_INFINITE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
                create.maxRenderSize = {m_outputSize.width, m_outputSize.height}; // jusqu'à FSR Natif AA
                create.maxUpscaleSize = {m_outputSize.width, m_outputSize.height};
                create.fpMessage = OnMessage;
                return m_functions.createContext(&m_context, &create.header, nullptr) == FFX_API_RETURN_OK;
            }

            std::uint32_t GetJitterPhaseCount(std::uint32_t renderWidth) const override {
                int32_t count = 8;
                ffxQueryDescUpscaleGetJitterPhaseCount query{};
                query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
                query.renderWidth = renderWidth;
                query.displayWidth = m_outputSize.width;
                query.pOutPhaseCount = &count;
                m_functions.query(const_cast<ffxContext*>(&m_context), &query.header);
                return static_cast<std::uint32_t>(std::max(count, 1));
            }

            void Dispatch(RHI::CommandList& commands, const RHI::UpscaleInputs& inputs) override {
                ffxDispatchDescUpscale dispatch{};
                dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
                dispatch.commandList = static_cast<RhiCommandList&>(commands).GetHandle();
                dispatch.color = Describe(*inputs.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
                dispatch.depth = Describe(*inputs.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
                dispatch.motionVectors = Describe(*inputs.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
                dispatch.output = Describe(*inputs.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
                if (inputs.reactive) {
                    dispatch.reactive = Describe(*inputs.reactive, FFX_API_RESOURCE_STATE_COMPUTE_READ);
                }
                dispatch.jitterOffset = {inputs.jitterX, inputs.jitterY};
                // Mouvement en fraction de l'image (uv) : FSR le veut en pixels de rendu.
                dispatch.motionVectorScale = {static_cast<float>(inputs.renderSize.width),
                                              static_cast<float>(inputs.renderSize.height)};
                dispatch.renderSize = {inputs.renderSize.width, inputs.renderSize.height};
                dispatch.upscaleSize = {m_outputSize.width, m_outputSize.height};
                dispatch.enableSharpening = inputs.sharpness > 0.0f;
                dispatch.sharpness = inputs.sharpness;
                dispatch.frameTimeDelta = inputs.frameMilliseconds;
                dispatch.preExposure = 1.0f;
                dispatch.reset = inputs.reset;
                // Plan lointain à l'infini : FSR prend le plus petit des deux comme plan proche.
                dispatch.cameraNear = inputs.nearPlane;
                dispatch.cameraFar = FLT_MAX;
                dispatch.cameraFovAngleVertical = inputs.verticalFov;
                dispatch.viewSpaceToMetersFactor = 1.0f;
                if (m_functions.dispatch(&m_context, &dispatch.header) != FFX_API_RETURN_OK && !m_reported) {
                    Log::Error("FSR : l'agrandissement de l'image a échoué");
                    m_reported = true;
                }
            }

            private:
            SharedLibrary m_library; // déchargée après la destruction du contexte (ordre des membres)
            FfxFunctions m_functions;
            RHI::Extent m_outputSize;
            ffxContext m_context = nullptr;
            bool m_reported = false; // une erreur de dispatch dite une fois, pas à chaque image
        };
    }

    std::expected<std::unique_ptr<RHI::Upscaler>, std::string> CreateFsrUpscaler(const Device& device, RHI::Extent outputSize) {
        auto library = SharedLibrary::Load(Platform::GetExecutableDirectory() / LibraryName);
        if (!library) {
            return std::unexpected(std::format("{} introuvable à côté de l'exe ({})", LibraryName, library.error()));
        }
        const FfxFunctions functions{library->GetFunction<PfnFfxCreateContext>("ffxCreateContext"),
                                            library->GetFunction<PfnFfxDestroyContext>("ffxDestroyContext"),
                                            library->GetFunction<PfnFfxQuery>("ffxQuery"),
                                            library->GetFunction<PfnFfxDispatch>("ffxDispatch")};
        if (!functions.createContext || !functions.destroyContext || !functions.query || !functions.dispatch) {
            return std::unexpected(std::format("{} : fonctions de l'API FidelityFX absentes", LibraryName));
        }
        auto upscaler = std::make_unique<FsrUpscaler>(std::move(*library), functions, outputSize);
        if (!upscaler->Init(device)) {
            return std::unexpected("FSR refusé par ce GPU ou ce pilote (création du contexte)");
        }
        return upscaler;
    }
}
