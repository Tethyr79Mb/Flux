#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Color.hpp"

#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace Flux {
    class Window;

    namespace UI {
        class DrawList;
    }

    // Aucun type Vulkan ici : tout vit dans Impl (Renderer.cpp).
    class FLUX_API Renderer {
        public:
        static std::expected<std::unique_ptr<Renderer>, std::string> Create(Window& window);

        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        // Retourne false sur une erreur irrécupérable (device perdu, etc.).
        bool RenderFrame();

        void SetClearColor(Math::LinearColor color);

        // Overlay 2D de la frame en cours, dessiné par-dessus la scène puis vidé après chaque RenderFrame.
        UI::DrawList& GetOverlay();

        // Nom du GPU utilisé, tel que rapporté par le pilote.
        std::string_view GetDeviceName() const;

        private:
        struct Impl;
        explicit Renderer(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> m_impl;
    };
}
