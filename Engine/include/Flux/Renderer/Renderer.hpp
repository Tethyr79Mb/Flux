#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Color.hpp"

#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace Flux {
    class Window;
    struct Camera;
    class Scene;

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
        bool RenderFrame(const Scene& scene, const Camera& camera);

        void SetClearColor(Math::LinearColor color);

        // V-Sync (activée par défaut) : sans elle, autant d'images que le GPU peut en produire.
        // Le changement prend effet à la frame suivante (la swapchain est recréée).
        void SetVSync(bool enabled);
        bool IsVSyncEnabled() const;
        // Mode de présentation Vulkan réellement utilisé, lisible (FIFO, MAILBOX...).
        std::string_view GetPresentModeName() const;

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
