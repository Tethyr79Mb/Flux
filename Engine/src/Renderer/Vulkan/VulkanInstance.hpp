#pragma once

#include "Renderer/Vulkan/Vulkan.hpp"

#include <VkBootstrap.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace Flux::Vk {
    // Instance Vulkan (chargée par volk), couches de validation en Debug et extension des repères nommés.
    class Instance {
        public:
        // headless : sans les extensions de fenêtre (lavapipe sous Windows, le GPU logiciel de la CI, n'a pas
        // VK_KHR_win32_surface).
        static std::expected<std::unique_ptr<Instance>, std::string> Create(bool headless);
        ~Instance();

        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;

        VkInstance GetHandle() const { return m_instance.instance; }
        const vkb::Instance& Get() const { return m_instance; }
        // Version demandée (1.3) : le device ne la dépasse pas.
        std::uint32_t GetApiVersion() const { return m_instance.api_version; }
        // VK_EXT_debug_utils activée : repères nommés dans les listes de commandes (sinon ses fonctions sont interdites).
        bool HasDebugLabels() const { return m_debugLabels; }

        private:
        Instance() = default;

        vkb::Instance m_instance;
        bool m_debugLabels = false;
    };
}
