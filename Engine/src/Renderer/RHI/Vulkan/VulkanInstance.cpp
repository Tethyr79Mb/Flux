#include "Renderer/RHI/Vulkan/VulkanInstance.hpp"

#include <cstdio>
#include <cstring>
#include <format>

namespace Flux::Vk {
#ifndef NDEBUG
    namespace {
        // Comme le messager par défaut de vk-bootstrap, sans un avertissement connu de FSR : sa ressource interne
        // rw_luma_history est créée en RGBA16F par la DLL d'AMD, mais son shader la déclare en RGBA8. Rien à corriger
        // de notre côté, et ces lignes, répétées à chaque image, noyaient les vrais messages.
        VKAPI_ATTR VkBool32 VKAPI_CALL OnDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                      VkDebugUtilsMessageTypeFlagsEXT type,
                                                      const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
            const char* id = data->pMessageIdName ? data->pMessageIdName : "";
            if (std::strcmp(id, "Undefined-Value-StorageImage-FormatMismatch-ImageView") == 0
                && std::strstr(data->pMessage, "rw_luma_history")) {
                return VK_FALSE;
            }
            std::printf("[%s: %s] - %s\n%s\n", vkb::to_string_message_severity(severity),
                        vkb::to_string_message_type(type), id, data->pMessage);
            return VK_FALSE;
        }
    }
#endif

    std::expected<std::unique_ptr<Instance>, std::string> Instance::Create(bool headless) {
        if (volkInitialize() != VK_SUCCESS) {
            return std::unexpected("vulkan-1.dll introuvable : installe ou mets à jour le pilote graphique");
        }
        // Le constructeur est privé : pas de std::make_unique.
        std::unique_ptr<Instance> instance(new Instance());

        vkb::InstanceBuilder builder(vkGetInstanceProcAddr);
        builder.set_app_name("Flux")
               .set_engine_name("FluxEngine")
               .set_headless(headless)
               .require_api_version(1, 3, 0);
#ifndef NDEBUG
        // Actives seulement si le SDK Vulkan est installé ; sinon on continue sans.
        builder.request_validation_layers(true)
               .set_debug_callback(OnDebugMessage);
#else
        // Noms des étapes (BeginStep) visibles dans Radeon GPU Profiler, Nsight ou RenderDoc, même en Release.
        if (const auto system = vkb::SystemInfo::get_system_info(vkGetInstanceProcAddr);
            system && system->is_extension_available(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            builder.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            instance->m_debugLabels = true;
        }
#endif
        auto built = builder.build();
        if (!built) {
            return std::unexpected(std::format("création de l'instance Vulkan : {}", built.error().message()));
        }
        instance->m_instance = built.value();
#ifndef NDEBUG
        instance->m_debugLabels = instance->m_instance.debug_messenger != VK_NULL_HANDLE; // le messager exige l'extension
#endif
        volkLoadInstance(instance->m_instance.instance);
        return instance;
    }

    Instance::~Instance() {
        if (m_instance.instance != VK_NULL_HANDLE) {
            vkb::destroy_instance(m_instance);
        }
    }
}
