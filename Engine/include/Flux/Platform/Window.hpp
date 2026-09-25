#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector2.hpp"

#include <memory>
#include <string_view>

namespace Flux {
    // Aucun header système ici : le HWND vit dans Impl (Win32Window.cpp).
    class FLUX_API Window {
        public:
        Window();
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;
        Window(Window&&) noexcept;
        Window& operator=(Window&&) noexcept;

        bool Create(std::string_view title, Math::Vector2i size);
        void Destroy();

        void HandleEvents();
        bool IsOpen() const;

        void SetTitle(std::string_view title);
        void SetSize(Math::Vector2i size);
        Math::Vector2i GetSize() const;

        // HWND sous Windows, pour vkCreateWin32SurfaceKHR.
        void* GetNativeHandle() const;

        private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
