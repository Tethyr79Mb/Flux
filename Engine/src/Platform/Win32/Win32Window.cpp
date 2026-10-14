#include "Flux/Platform/Window.hpp"

#include "Platform/Win32/Win32Utf.hpp"

#include <utility>

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
        Math::Vector2i restoredSize; // taille hors agrandissement : celle à rendre quand la fenêtre est restaurée
        Input input;
        bool focused = false;
        bool cursorLocked = false;
        bool closeRequested = false;
        bool fullscreen = false;
        WINDOWPLACEMENT windowedPlacement{}; // place, taille et état (agrandie) à rendre en quittant le plein écran
        MouseCursor cursor = MouseCursor::Arrow;
        wchar_t highSurrogate = 0; // WM_CHAR livre les caractères hors BMP en deux moitiés UTF-16

        // Fenêtre sans cadre posée sur tout l'écran qui la contient ; showWindow pour la première apparition.
        void EnterFullscreen(bool showWindow) {
            windowedPlacement.length = sizeof(windowedPlacement);
            GetWindowPlacement(hwnd, &windowedPlacement);
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
            fullscreen = true;
            SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | (showWindow ? 0 : WS_VISIBLE));
            const RECT& area = monitor.rcMonitor;
            SetWindowPos(hwnd, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top,
                         SWP_FRAMECHANGED | SWP_NOOWNERZORDER | (showWindow ? SWP_SHOWWINDOW : 0));
        }

        void LeaveFullscreen() {
            fullscreen = false;
            SetWindowLongPtrW(hwnd, GWL_STYLE, WindowStyle | WS_VISIBLE);
            SetWindowPlacement(hwnd, &windowedPlacement);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }

        void LockCursor(bool locked) {
            if (locked == cursorLocked) {
                return;
            }
            cursorLocked = locked;
            if (locked) {
                // Le curseur reste où il est, enfermé dans un rectangle d'un pixel.
                POINT position;
                GetCursorPos(&position);
                const RECT clip{position.x, position.y, position.x + 1, position.y + 1};
                ClipCursor(&clip);
                ShowCursor(FALSE);
            } else {
                ClipCursor(nullptr);
                ShowCursor(TRUE);
            }
        }

        static HCURSOR CursorHandle(MouseCursor cursor) {
            switch (cursor) {
                case MouseCursor::ResizeHorizontal:
                    return LoadCursorW(nullptr, IDC_SIZEWE);
                case MouseCursor::ResizeVertical:
                    return LoadCursorW(nullptr, IDC_SIZENS);
                case MouseCursor::ResizeNwse:
                    return LoadCursorW(nullptr, IDC_SIZENWSE);
                case MouseCursor::ResizeNesw:
                    return LoadCursorW(nullptr, IDC_SIZENESW);
                case MouseCursor::Move:
                    return LoadCursorW(nullptr, IDC_SIZEALL);
                case MouseCursor::Arrow:
                    break;
            }
            return LoadCursorW(nullptr, IDC_ARROW);
        }

        bool MouseInClient() const {
            POINT position;
            RECT client;
            return GetCursorPos(&position) && ScreenToClient(hwnd, &position) && GetClientRect(hwnd, &client)
                && PtInRect(&client, position);
        }

        static void RegisterRawMouse(HWND hwnd) {
            // Souris brute : déplacements sans accélération ni blocage aux bords de l'écran.
            RAWINPUTDEVICE device{};
            device.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
            device.usUsage = 0x02;     // HID_USAGE_GENERIC_MOUSE
            device.hwndTarget = hwnd;
            RegisterRawInputDevices(&device, 1, sizeof(device));
        }

        void OnRawInput(LPARAM lParam) {
            RAWINPUT raw;
            UINT dataSize = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &dataSize, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
                return;
            }
            // Les tablettes et le bureau à distance envoient des positions absolues : ignorées pour l'instant.
            if (raw.header.dwType == RIM_TYPEMOUSE && (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                input.AddMouseDelta(static_cast<float>(raw.data.mouse.lLastX), static_cast<float>(raw.data.mouse.lLastY));
            }
        }

        void OnKey(WPARAM wParam, LPARAM lParam, bool down) {
            // Lettre imprimée sur la touche, selon la disposition du clavier (raccourcis Ctrl+Z...) ; pas les répétitions.
            if (down && (lParam & (1 << 30)) == 0 && wParam >= 'A' && wParam <= 'Z') {
                input.SetLetterPressed(static_cast<char>(wParam));
            }
            auto scancode = static_cast<std::uint16_t>((lParam >> 16) & 0xFF);
            if (lParam & (1 << 24)) {
                scancode |= 0x100; // touche étendue (flèches, Ctrl droit...)
            }
            input.SetKey(scancode, down);
        }

        void OnMouseButton(MouseButton button, bool down) {
            input.SetMouseButton(button, down);
            // Capture tant qu'un bouton est enfoncé : on reçoit le relâchement même hors de la fenêtre
            // (glisser une valeur ou un panneau jusqu'au bord de l'écran).
            const bool anyDown = input.IsMouseButtonDown(MouseButton::Left) || input.IsMouseButtonDown(MouseButton::Right)
                              || input.IsMouseButtonDown(MouseButton::Middle);
            if (anyDown && GetCapture() != hwnd) {
                SetCapture(hwnd);
            } else if (!anyDown && GetCapture() == hwnd) {
                ReleaseCapture();
            }
        }

        void OnChar(WPARAM wParam) {
            const auto unit = static_cast<wchar_t>(wParam);
            if (unit >= 0xD800 && unit <= 0xDBFF) {
                highSurrogate = unit;
                return;
            }
            if (unit >= 0xDC00 && unit <= 0xDFFF) {
                if (highSurrogate != 0) {
                    input.AddCharacter(0x10000 + ((static_cast<char32_t>(highSurrogate) - 0xD800) << 10) + (unit - 0xDC00));
                }
                highSurrogate = 0;
                return;
            }
            highSurrogate = 0;
            input.AddCharacter(unit);
        }

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
                        if (wParam == SIZE_RESTORED && !impl->fullscreen) {
                            impl->restoredSize = impl->size;
                        }
                        return 0;
                    case WM_SETFOCUS:
                        impl->focused = true;
                        return 0;
                    case WM_KILLFOCUS:
                        impl->focused = false;
                        impl->input.ReleaseAll();
                        impl->LockCursor(false);
                        return 0;
                    case WM_INPUT:
                        impl->OnRawInput(lParam);
                        break; // DefWindowProc libère les données brutes
                    case WM_KEYDOWN:
                    case WM_SYSKEYDOWN:
                        impl->OnKey(wParam, lParam, true);
                        break; // DefWindowProc garde Alt+F4
                    case WM_KEYUP:
                    case WM_SYSKEYUP:
                        impl->OnKey(wParam, lParam, false);
                        break;
                    case WM_CHAR:
                        impl->OnChar(wParam);
                        return 0;
                    case WM_MOUSEMOVE:
                        // Coordonnées signées : négatives hors de la fenêtre pendant une capture.
                        impl->input.SetMousePosition({static_cast<float>(static_cast<short>(LOWORD(lParam))),
                                                      static_cast<float>(static_cast<short>(HIWORD(lParam)))});
                        return 0;
                    case WM_LBUTTONDOWN:
                    case WM_LBUTTONUP:
                        impl->OnMouseButton(MouseButton::Left, message == WM_LBUTTONDOWN);
                        return 0;
                    case WM_RBUTTONDOWN:
                    case WM_RBUTTONUP:
                        impl->OnMouseButton(MouseButton::Right, message == WM_RBUTTONDOWN);
                        return 0;
                    case WM_MBUTTONDOWN:
                    case WM_MBUTTONUP:
                        impl->OnMouseButton(MouseButton::Middle, message == WM_MBUTTONDOWN);
                        return 0;
                    case WM_CAPTURECHANGED:
                        // Capture perdue (Alt+Tab, boîte de dialogue) : les relâchements ne viendront pas.
                        if (reinterpret_cast<HWND>(lParam) != hwnd) {
                            impl->input.SetMouseButton(MouseButton::Left, false);
                            impl->input.SetMouseButton(MouseButton::Right, false);
                            impl->input.SetMouseButton(MouseButton::Middle, false);
                        }
                        return 0;
                    case WM_MOUSEWHEEL:
                        impl->input.AddWheelDelta(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA);
                        return 0;
                    case WM_SETCURSOR:
                        // Dans la zone client, la forme choisie par le programme ; sur les bords, celle de Windows.
                        if (LOWORD(lParam) == HTCLIENT) {
                            SetCursor(CursorHandle(impl->cursor));
                            return TRUE;
                        }
                        break;
                    case WM_MENUCHAR:
                        return MAKELRESULT(0, MNC_CLOSE); // Alt+Entrée (plein écran) : pas de bip « pas de menu »
                    case WM_CLOSE:
                        impl->closeRequested = true; // Engine demande confirmation si besoin, puis détruit la fenêtre
                        return 0;
                    case WM_DESTROY:
                        impl->LockCursor(false);
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

    bool Window::Create(std::string_view title, Math::Vector2i size, bool maximized, bool fullscreen) {
        if (!m_impl || m_impl->hwnd || !Impl::RegisterWindowClass()) {
            return false;
        }

        const SIZE outer = OuterSize(size);
        m_impl->size = size;
        m_impl->restoredSize = size;
        m_impl->hwnd = CreateWindowExW(
            0, WindowClassName, Win32::ToWide(title).c_str(), WindowStyle,
            CW_USEDEFAULT, CW_USEDEFAULT, outer.cx, outer.cy,
            nullptr, nullptr, GetModuleHandleW(nullptr), m_impl.get());
        if (!m_impl->hwnd) {
            return false;
        }

        Impl::RegisterRawMouse(m_impl->hwnd);
        if (fullscreen) {
            // Encore cachée : la place fenêtrée (agrandie ou non) est notée, puis la fenêtre apparaît en plein écran.
            m_impl->EnterFullscreen(true);
            m_impl->windowedPlacement.showCmd = maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
            return true;
        }
        // Maximisée : WM_SIZE donne la taille réelle ; size ne sert qu'une fois la fenêtre restaurée.
        ShowWindow(m_impl->hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
        return true;
    }

    void Window::Destroy() {
        if (m_impl && m_impl->hwnd) {
            DestroyWindow(m_impl->hwnd);
        }
    }

    void Window::HandleEvents() {
        if (m_impl) {
            m_impl->input.BeginFrame();
        }
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

    bool Window::ConsumeCloseRequest() {
        return m_impl && std::exchange(m_impl->closeRequested, false);
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

    Math::Vector2i Window::GetRestoredSize() const {
        return m_impl ? m_impl->restoredSize : Math::Vector2i{};
    }

    bool Window::IsMaximized() const {
        if (!IsOpen()) {
            return false;
        }
        return m_impl->fullscreen ? m_impl->windowedPlacement.showCmd == SW_SHOWMAXIMIZED : IsZoomed(m_impl->hwnd) != FALSE;
    }

    void Window::SetFullscreen(bool fullscreen) {
        if (IsOpen() && fullscreen != m_impl->fullscreen) {
            if (fullscreen) {
                m_impl->EnterFullscreen(false);
            } else {
                m_impl->LeaveFullscreen();
            }
        }
    }

    bool Window::IsFullscreen() const {
        return IsOpen() && m_impl->fullscreen;
    }

    const Input& Window::GetInput() const {
        return m_impl->input;
    }

    bool Window::HasFocus() const {
        return m_impl && m_impl->focused;
    }

    void Window::SetCursorLocked(bool locked) {
        if (IsOpen()) {
            m_impl->LockCursor(locked && m_impl->focused);
        }
    }

    bool Window::IsCursorLocked() const {
        return m_impl && m_impl->cursorLocked;
    }

    void Window::SetMouseCursor(MouseCursor cursor) {
        if (!IsOpen() || cursor == m_impl->cursor) {
            return;
        }
        m_impl->cursor = cursor;
        // Sans attendre que la souris bouge (WM_SETCURSOR) : elle peut être immobile au bord d'une fenêtre.
        if (m_impl->MouseInClient()) {
            SetCursor(Impl::CursorHandle(cursor));
        }
    }

    void* Window::GetNativeHandle() const {
        return m_impl ? m_impl->hwnd : nullptr;
    }
}
