#pragma once

#include "Flux/Assets/Image.hpp"
#include "Flux/Export.hpp"
#include "Flux/Math/Box3.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Scene/Components.hpp"
#include "Flux/UI/DrawList.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Flux {
    class Window;
    struct Camera;
    class Scene;

    // Temps de la dernière image rendue, en millisecondes. Les temps du GPU sont ceux d'une image un peu plus ancienne
    // (relus quand il l'a finie) ; négatifs s'ils sont inconnus (GPU sans heures, ou pas encore mesurés).
    struct RenderTimings
    {
        double prepare = 0.0;  // CPU : matrices, poses, lumières, cascades, envoi des données de l'image
        double record = 0.0;   // CPU : enregistrement des commandes de dessin et soumission
        double waitGpu = 0.0;  // CPU arrêté : le GPU n'a pas fini l'image qui utilisait les mêmes ressources
        double waitImage = 0.0; // CPU arrêté : aucune image de la fenêtre libre (V-Sync, plafond du pilote)
        double present = 0.0;  // CPU : appel de présentation
        double gpuShadows = -1.0;   // GPU : carte d'ombres
        double gpuScene = -1.0;     // GPU : maillages de la scène
        double gpuInterface = -1.0; // GPU : interface (overlay 2D)
    };

    // Aucun type d'API graphique ici : le rendu passe par le RHI (src/Renderer/RHI), implémenté avec Vulkan.
    class FLUX_API Renderer {
        public:
        static std::expected<std::unique_ptr<Renderer>, std::string> Create(Window& window);

        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        // Retourne false sur une erreur irrécupérable (device perdu, etc.).
        // `hidden` et ses descendants ne sont pas dessinés : le personnage quand la caméra est dans sa tête.
        bool RenderFrame(const Scene& scene, const Camera& camera, Entity hidden = NullEntity);

        void SetClearColor(Math::LinearColor color);

        // Partie de la fenêtre où la scène est dessinée (en pixels, la vue 3D de l'éditeur) ; l'image garde les
        // proportions de ce rectangle. min == max (par défaut) : toute la fenêtre. L'overlay 2D couvre toujours tout.
        void SetSceneViewport(Math::Vector2f min, Math::Vector2f max);

        // V-Sync (activée par défaut) : sans elle, autant d'images que le GPU peut en produire.
        // Le changement prend effet à la frame suivante (la swapchain est recréée).
        void SetVSync(bool enabled);
        bool IsVSyncEnabled() const;
        // Mode de présentation réellement utilisé, lisible (FIFO, MAILBOX...).
        std::string_view GetPresentModeName() const;

        // Overlay 2D de la frame en cours, dessiné par-dessus la scène puis vidé après chaque RenderFrame.
        UI::DrawList& GetOverlay();

        // Image (PNG, JPEG, TGA, BMP) à dessiner dans l'overlay (DrawList::AddImage), chargée au premier appel puis
        // gardée jusqu'à la fermeture ; appeler à chaque frame est prévu : c'est une recherche dans un cache, et le
        // fichier est relu s'il a changé (vérifié une fois par seconde). nullopt si le fichier est absent ou illisible
        // (message dans la console). Au plus 256 images différentes.
        std::optional<UI::TextureInfo> LoadTexture(const std::filesystem::path& path);
        // Image déjà en mémoire (icône du système...) : une nouvelle texture à chaque appel, gardée jusqu'à la
        // fermeture ; l'appelant garde le résultat. Compte dans les 256 images. nullopt si l'image est invalide.
        std::optional<UI::TextureInfo> CreateTexture(const ImageData& image);

        // Dossier des assets du projet : les modèles de la scène (ModelComponent) y sont cherchés. Vide (sans projet) :
        // aucun modèle dessiné, les cubes seulement.
        void SetAssetsDirectory(const std::filesystem::path& directory);

        // Modèle glTF (.gltf, .glb), chargé au premier appel puis gardé jusqu'à la fermeture, et relu si le fichier
        // change (vérifié une fois par seconde) ; c'est aussi ce que fait le rendu pour chaque ModelComponent.
        // Retourne sa boîte englobante (repère du modèle) ; nullopt si le fichier est absent ou illisible
        // (message dans la console).
        std::optional<Math::Box3f> LoadModel(const std::filesystem::path& path);
        // Noms des animations du modèle, dans l'ordre du fichier (chargé au besoin) ; vide s'il n'en a pas.
        std::vector<std::string> GetAnimationNames(const std::filesystem::path& path);
        // Durée d'une animation de ce modèle, en secondes ; 0 si le modèle n'est pas chargé ou ne l'a pas.
        float GetAnimationDuration(const std::filesystem::path& path, std::string_view name);

        const RenderTimings& GetTimings() const;

        // Nom du GPU utilisé, tel que rapporté par le pilote.
        std::string_view GetDeviceName() const;

        private:
        struct Impl;
        explicit Renderer(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> m_impl;
    };
}
