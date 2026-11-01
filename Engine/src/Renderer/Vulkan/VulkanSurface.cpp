#include "Renderer/Vulkan/VulkanSurface.hpp"

#include "Renderer/Vulkan/VulkanInstance.hpp"

#include <windows.h>

namespace Flux::Vk {
    std::expected<std::unique_ptr<Surface>, std::string> Surface::Create(const Instance& instance, void* nativeWindowHandle) {
        // Le constructeur est privé : pas de std::make_unique.
        std::unique_ptr<Surface> surface(new Surface(instance));
        VkWin32SurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        info.hinstance = GetModuleHandleW(nullptr);
        info.hwnd = static_cast<HWND>(nativeWindowHandle);
        if (!Check(vkCreateWin32SurfaceKHR(instance.GetHandle(), &info, nullptr, &surface->m_surface), "vkCreateWin32SurfaceKHR")) {
            return std::unexpected("création de la surface Vulkan");
        }
        return surface;
    }

    Surface::~Surface() {
        if (m_surface != VK_NULL_HANDLE) {
            vkb::destroy_surface(m_instance.Get(), m_surface);
        }
    }
}
