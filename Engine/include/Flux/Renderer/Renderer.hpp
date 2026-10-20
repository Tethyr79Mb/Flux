#pragma once

#include "Flux/Assets/Image.hpp"
#include "Flux/Export.hpp"
#include "Flux/Math/Box3.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Scene/Components.hpp"
#include "Flux/UI/DrawList.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Flux {
    class Window;
    struct Camera;
    class Scene;
    struct Projectile;
    struct Impact;

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
        double gpuScene = -1.0;     // GPU : maillages de la scène, somme des sept suivants
        double gpuObjects = -1.0;   // GPU : entités opaques
        double gpuVegetation = -1.0; // GPU : arbres des planètes
        double gpuGround = -1.0;    // GPU : sol des planètes
        double gpuSky = -1.0;       // GPU : fond du ciel
        double gpuOcean = -1.0;     // GPU : océans
        double gpuClouds = -1.0;    // GPU : nuages des planètes
        double gpuTransparent = -1.0; // GPU : entités transparentes
        double gpuPyramid = -1.0;   // GPU : pyramide de profondeur (entités cachées écartées à l'image suivante)
        double gpuTemporal = -1.0;  // GPU : anticrénelage temporel
        double gpuBloom = -1.0;     // GPU : bloom et image finale (courbe de tons)
        double gpuInterface = -1.0; // GPU : interface (overlay 2D)
        std::uint32_t planetTiles = 0;       // tuiles de planète dessinées
        std::uint32_t planetOceanTiles = 0;  // dont redessinées en eau
        std::uint32_t planetStoredTiles = 0; // gardées en mémoire vidéo
        std::uint32_t planetTileCapacity = 0; // au plus, en mémoire vidéo
        // Arbres des planètes deux images plus tôt : dessinés (détaillés, simplifiés, lointains) et dans les ombres ;
        // plaines triées à la dernière image, et en vue pas encore semées.
        std::array<std::uint32_t, 3> trees{};
        std::uint32_t shadowTrees = 0;
        std::uint32_t treePlains = 0;
        std::uint32_t pendingTreePlains = 0;
        // Chargement encore en cours (écran de chargement) : tuiles demandées pas encore prêtes, textures du sol pas
        // encore décodées, modèles pas encore lus ou envoyés au GPU, polices dont l'atlas se construit.
        std::uint32_t planetPendingTiles = 0;
        bool groundTexturesLoading = false;
        std::uint32_t modelsLoading = 0;
        std::uint32_t fontsLoading = 0;
        // Entités (et blocs de grands modèles) de la passe principale testées par le GPU, et dont cachées derrière
        // d'autres (non dessinées), deux images plus tôt.
        std::uint32_t occlusionTested = 0;
        std::uint32_t occlusionHidden = 0;
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
        // false : ni ombres, ni scène 3D, ni ciel ; seule l'interface est dessinée (l'éditeur d'interface couvre la vue).
        void SetSceneVisible(bool visible);
        // Flammes des réacteurs montrées à pleine poussée, quels que soient les moteurs : dans l'éditeur, jeu arrêté,
        // pour les voir et les régler (sinon, au ralenti, elles sont à peine visibles).
        void SetThrusterPreview(bool preview);
        // Projectiles à dessiner en traçantes, et éclats d'impact, aux prochains RenderFrame (Weapons). Les tableaux
        // ne sont pas copiés : ils doivent rester valides jusqu'au prochain appel (vides : rien).
        void SetProjectiles(std::span<const Projectile> projectiles, std::span<const Impact> impacts = {});

        // V-Sync (activée par défaut) : sans elle, autant d'images que le GPU peut en produire.
        // Le changement prend effet à la frame suivante (la swapchain est recréée).
        void SetVSync(bool enabled);
        bool IsVSyncEnabled() const;
        // Force du bloom, de 0 (aucun) à 1 : la part de la lumière de chaque pixel étalée autour de lui. Les zones très
        // lumineuses (soleil, flammes, émissifs) débordent d'autant plus qu'elles sont fortes.
        static constexpr float DefaultBloom = 0.04f;
        void SetBloom(float strength);
        float GetBloom() const;
        // Anticrénelage temporel (activé par défaut) : chaque image est décalée d'une fraction de pixel, puis mélangée
        // aux précédentes ; les bords en escalier et le grain des nuages s'effacent.
        void SetTemporalAA(bool enabled);
        bool IsTemporalAAEnabled() const;
        // Entités cachées derrière d'autres écartées par le GPU (activé par défaut), d'après la profondeur de l'image
        // précédente. Désactivable pour comparer.
        void SetOcclusionCulling(bool enabled);
        // Grille au sol de l'éditeur (plan y = 0, lignes du mètre, des 10 m et des 100 m, axes X et Z en couleur),
        // cachée derrière les objets ; désactivée par défaut, le jeu ne la montre pas.
        void SetGridVisible(bool visible);
        bool IsGridVisible() const;
        bool IsOcclusionCullingEnabled() const;
        // Mesure (désactivée par défaut) : le sol des planètes dessiné sans le calcul de ses pixels, gris. L'écart de
        // « dont sol » dans le panneau Moteur donne le coût des pixels ; le reste, celui des sommets et des triangles.
        void SetGroundGeometryOnly(bool enabled);
        bool IsGroundGeometryOnly() const;
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
        // Police (.ttf, .otf) pour DrawList::AddText, gardée jusqu'à la fermeture ; appeler à chaque frame est prévu.
        // Le premier appel lance la construction de son atlas MSDF sur un autre fil (0,3 à 0,5 s par police) et
        // retourne nullopt jusqu'à ce qu'il soit prêt : le texte s'affiche dans la police de l'interface en attendant
        // (RenderTimings::fontsLoading le compte, l'écran de chargement l'attend). Pas relue si le fichier change.
        // nullopt aussi si le fichier est absent ou illisible (message dans la console, une fois). Compte dans les
        // 256 images.
        std::optional<UI::FontRef> LoadFont(const std::filesystem::path& path);

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
        // Temps du GPU étape par étape pour la prochaine image : à demander à chaque image où on les affiche (panneau
        // Moteur). Sans, le GPU ne note aucune heure (GetTimings donne -1) : celles prises au milieu d'une passe
        // l'empêchent d'enchaîner ses étapes.
        void RequestDetailedTimings();

        // Nom du GPU utilisé, tel que rapporté par le pilote.
        std::string_view GetDeviceName() const;
        // Version de Vulkan utilisée, puis celle que le GPU accepte : « 1.3 (GPU : 1.4.303) ».
        std::string_view GetApiVersion() const;
        // Octets réservés par le rendu en mémoire vidéo (first) et en RAM lisible par le GPU (second).
        std::pair<std::uint64_t, std::uint64_t> GetGpuMemory() const;

        private:
        struct Impl;
        explicit Renderer(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> m_impl;
    };
}
