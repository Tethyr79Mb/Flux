#include "Flux/Platform/Window.hpp"

#include "Platform/Win32/Win32Utf.hpp"

namespace Flux {
    namespace {
        constexpr wchar_t WindowClassName[] = L"FluxWindow";
        constexpr DWORD WindowStyle = WS_OVERLAPPEDWINDOW;

        // Taille de la fenêtre complète pour obtenir une zone client de clientSize.
        SIZE OuterSize(Math::Vector2i clientSize) {
            RECT rect{0, 0, clientSize.x, clientSize.y};
            AdjustWindowRectEx(&rect, WindowStyle, FALSE, 0);
            return {rect.right - rect.left, rect.bottom - rect.top};
        }
    }

    struct Window::Impl {
        HWND hwnd = nullptr;
        Math::Vector2i size;

        static bool RegisterWindowClass() {
            static const bool registered = [] {
                WNDCLASSEXW windowClass{};
                windowClass.cbSize = sizeof(windowClass);
                windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
                windowClass.lpfnWndProc = Proc;
                windowClass.hInstance = GetModuleHandleW(nullptr);
                windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                windowClass.lpszClassName = WindowClassName;
                return RegisterClassExW(&windowClass) != 0;
            }();
            return registered;
        }

        static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
            if (message == WM_NCCREATE) {
                const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            }

            auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (impl) {
                switch (message) {
                    case WM_SIZE:
                        impl->size = {LOWORD(lParam), HIWORD(lParam)};
                        return 0;
                    case WM_CLOSE:
                        DestroyWindow(hwnd);
                        return 0;
                    case WM_DESTROY:
                        impl->hwnd = nullptr;
                        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                        return 0;
                    default:
                        break;
                }
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    };

    Window::Window() : m_impl(std::make_unique<Impl>()) {}

    Window::~Window() {
        Destroy();
    }

    // Déplacer une fenêtre ouverte est sûr : la procédure de fenêtre pointe sur Impl, qui ne bouge pas.
    Window::Window(Window&&) noexcept = default;

    Window& Window::operator=(Window&& other) noexcept {
        if (this != &other) {
            Destroy();
            m_impl = std::move(other.m_impl);
        }
        return *this;
    }

    bool Window::Create(std::string_view title, Math::Vector2i size) {
        if (!m_impl || m_impl->hwnd || !Impl::RegisterWindowClass()) {
            return false;
        }

        const SIZE outer = OuterSize(size);
        m_impl->size = size;
        m_impl->hwnd = CreateWindowExW(
            0, WindowClassName, Win32::ToWide(title).c_str(), WindowStyle,
            CW_USEDEFAULT, CW_USEDEFAULT, outer.cx, outer.cy,
            nullptr, nullptr, GetModuleHandleW(nullptr), m_impl.get());
        if (!m_impl->hwnd) {
            return false;
        }

        ShowWindow(m_impl->hwnd, SW_SHOW);
        return true;
    }

    void Window::Destroy() {
        if (m_impl && m_impl->hwnd) {
            DestroyWindow(m_impl->hwnd);
        }
    }

    void Window::HandleEvents() {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    void Window::WaitEvents() {
        WaitMessage();
        HandleEvents();
    }

    bool Window::IsOpen() const {
        return m_impl && m_impl->hwnd;
    }

    void Window::SetTitle(std::string_view title) {
        if (IsOpen()) {
            SetWindowTextW(m_impl->hwnd, Win32::ToWide(title).c_str());
        }
    }

    void Window::SetSize(Math::Vector2i size) {
        if (IsOpen()) {
            const SIZE outer = OuterSize(size);
            SetWindowPos(m_impl->hwnd, nullptr, 0, 0, outer.cx, outer.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    Math::Vector2i Window::GetSize() const {
        return m_impl ? m_impl->size : Math::Vector2i{};
    }

    void* Window::GetNativeHandle() const {
        return m_impl ? m_impl->hwnd : nullptr;
    }
}
