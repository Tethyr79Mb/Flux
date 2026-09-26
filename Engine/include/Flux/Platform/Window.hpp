#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Platform/Input.hpp"

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

        // maximized : la fenêtre occupe tout l'écran (bureau) dès l'ouverture, barre des tâches exceptée.
        bool Create(std::string_view title, Math::Vector2i size, bool maximized = false);
        void Destroy();

        // Traite les messages en attente et met à jour GetInput().
        void HandleEvents();
        // Bloque jusqu'au prochain événement puis le traite (fenêtre réduite, par exemple).
        void WaitEvents();
        bool IsOpen() const;
        // Clic sur la croix (ou Alt+F4) : la fenêtre ne se ferme pas d'elle-même, le programme décide.
        // Retourne true une seule fois par demande.
        bool ConsumeCloseRequest();

        void SetTitle(std::string_view title);
        void SetSize(Math::Vector2i size);
        Math::Vector2i GetSize() const;

        const Input& GetInput() const;
        bool HasFocus() const;

        // Curseur caché et immobilisé (vol libre de la caméra) ; libéré automatiquement si la fenêtre perd le focus.
        void SetCursorLocked(bool locked);
        bool IsCursorLocked() const;

        // HWND sous Windows, pour vkCreateWin32SurfaceKHR.
        void* GetNativeHandle() const;

        private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
