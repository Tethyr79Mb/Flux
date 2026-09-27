#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/UI/DrawList.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Flux {
    class Window;
    struct Camera;
    class Scene;

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

        // Partie de la fenêtre où la scène est dessinée (en pixels, la vue 3D de l'éditeur) ; l'image garde les
        // proportions de ce rectangle. min == max (par défaut) : toute la fenêtre. L'overlay 2D couvre toujours tout.
        void SetSceneViewport(Math::Vector2f min, Math::Vector2f max);

        // V-Sync (activée par défaut) : sans elle, autant d'images que le GPU peut en produire.
        // Le changement prend effet à la frame suivante (la swapchain est recréée).
        void SetVSync(bool enabled);
        bool IsVSyncEnabled() const;
        // Mode de présentation Vulkan réellement utilisé, lisible (FIFO, MAILBOX...).
        std::string_view GetPresentModeName() const;

        // Overlay 2D de la frame en cours, dessiné par-dessus la scène puis vidé après chaque RenderFrame.
        UI::DrawList& GetOverlay();

        // Image (PNG, JPEG, TGA, BMP) à dessiner dans l'overlay (DrawList::AddImage), chargée au premier appel puis
        // gardée jusqu'à la fermeture ; appeler à chaque frame est prévu : c'est une recherche dans un cache, et le
        // fichier est relu s'il a changé (vérifié une fois par seconde). nullopt si le fichier est absent ou illisible
        // (message dans la console). Au plus 256 images différentes.
        std::optional<UI::TextureInfo> LoadTexture(const std::filesystem::path& path);

        // Nom du GPU utilisé, tel que rapporté par le pilote.
        std::string_view GetDeviceName() const;

        private:
        struct Impl;
        explicit Renderer(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> m_impl;
    };
}
