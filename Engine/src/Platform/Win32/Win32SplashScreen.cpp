#include "Flux/Platform/SplashScreen.hpp"

#include "Flux/Assets/Image.hpp"

#include "Platform/Win32/Win32Utf.hpp"

#include <cstdint>

namespace Flux::Platform {
    namespace {
        constexpr wchar_t SplashClassName[] = L"FluxSplashScreen";

        bool RegisterSplashClass() {
            static const bool registered = [] {
                WNDCLASSEXW windowClass{};
                windowClass.cbSize = sizeof(windowClass);
                windowClass.lpfnWndProc = DefWindowProcW;
                windowClass.hInstance = GetModuleHandleW(nullptr);
                windowClass.hCursor = LoadCursorW(nullptr, IDC_APPSTARTING);
                windowClass.lpszClassName = SplashClassName;
                return RegisterClassExW(&windowClass) != 0;
            }();
            return registered;
        }
    }

    struct SplashScreen::Impl
    {
        HWND window = nullptr;
    };

    SplashScreen::SplashScreen() : m_impl(std::make_unique<Impl>()) {}

    SplashScreen::~SplashScreen() {
        Close();
    }

    bool SplashScreen::Show(const std::filesystem::path& path) {
        Close();
        auto image = LoadImageFile(path);
        if (!image || !RegisterSplashClass()) {
            return false;
        }
        const int width = image->size.x;
        const int height = image->size.y;
        // Fenêtre en couches : UpdateLayeredWindow attend du BGRA à alpha prémultiplié.
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; // négatif : de haut en bas, comme l'image
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) {
            DeleteDC(memory);
            ReleaseDC(nullptr, screen);
            return false;
        }
        auto* out = static_cast<std::uint8_t*>(bits);
        const std::uint8_t* in = image->rgba.data();
        for (std::size_t i = 0; i < image->rgba.size(); i += 4) {
            const std::uint32_t alpha = in[i + 3];
            out[i] = static_cast<std::uint8_t>(in[i + 2] * alpha / 255);
            out[i + 1] = static_cast<std::uint8_t>(in[i + 1] * alpha / 255);
            out[i + 2] = static_cast<std::uint8_t>(in[i] * alpha / 255);
            out[i + 3] = static_cast<std::uint8_t>(alpha);
        }
        // Au milieu de l'écran principal (zone de travail : sans la barre des tâches).
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        POINT position{work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2};
        // Fenêtre d'outil : pas de bouton dans la barre des tâches (celui de l'éditeur arrive juste après).
        m_impl->window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, SplashClassName, L"", WS_POPUP,
                                         position.x, position.y, width, height, nullptr, nullptr, GetModuleHandleW(nullptr),
                                         nullptr);
        if (m_impl->window) {
            HGDIOBJ previous = SelectObject(memory, bitmap);
            SIZE size{width, height};
            POINT origin{0, 0};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(m_impl->window, screen, &position, &size, memory, &origin, 0, &blend, ULW_ALPHA);
            SelectObject(memory, previous);
            ShowWindow(m_impl->window, SW_SHOWNOACTIVATE);
            // Messages de l'affichage traités tout de suite : le programme ne relira les siens qu'une fois sa propre
            // fenêtre créée (sous Wine, l'écran d'accueil n'apparaîtrait qu'à ce moment).
            MSG message;
            while (PeekMessageW(&message, m_impl->window, 0, 0, PM_REMOVE)) {
                DispatchMessageW(&message);
            }
        }
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return m_impl->window != nullptr;
    }

    void SplashScreen::Close() {
        if (m_impl && m_impl->window) {
            DestroyWindow(m_impl->window);
            m_impl->window = nullptr;
        }
    }
}
