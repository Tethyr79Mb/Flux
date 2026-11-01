#include "Renderer/Vulkan/VulkanInstance.hpp"

#include <format>

namespace Flux::Vk {
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
               .use_default_debug_messenger();
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
