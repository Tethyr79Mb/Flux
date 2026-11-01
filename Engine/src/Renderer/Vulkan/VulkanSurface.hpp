#pragma once

#include "Renderer/Vulkan/Vulkan.hpp"

#include <expected>
#include <memory>
#include <string>

namespace Flux::Vk {
    class Instance;

    // Surface Vulkan de la fenêtre (HWND), où la swapchain présente ses images. Détruite avant son instance.
    class Surface {
        public:
        static std::expected<std::unique_ptr<Surface>, std::string> Create(const Instance& instance, void* nativeWindowHandle);
        ~Surface();

        Surface(const Surface&) = delete;
        Surface& operator=(const Surface&) = delete;

        VkSurfaceKHR GetHandle() const { return m_surface; }

        private:
        explicit Surface(const Instance& instance) : m_instance(instance) {}

        const Instance& m_instance;
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    };
}
