#pragma once

#include "Flux/Assets/Image.hpp"
#include "Flux/Export.hpp"
#include "Flux/Math/Box3.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Vector2.hpp"
#include "Flux/Planetology/CloudQuality.hpp"
#include "Flux/Planetology/GroundDetail.hpp"
#include "Flux/Planetology/VegetationDistance.hpp"
#include "Flux/Renderer/Upscaling.hpp"
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
    class JobSystem;
    class Window;
    struct Camera;
    class Scene;
    struct Projectile;
    struct Impact;

    // Temps du GPU d'une passe du rendu (step false) ou d'une de ses étapes, en millisecondes ; négatif si inconnu.
    // Pour une passe, aussi le temps CPU de son enregistrement (moyenne glissante), sur le fil qui l'a faite.
    struct RenderPassTiming
    {
        const char* name; // texte constant
        double milliseconds;
        bool step;
        double cpuMilliseconds = -1.0;
    };

    // Temps de la dernière image rendue, en millisecondes. Les temps du GPU sont ceux d'une image un peu plus ancienne
    // (relus quand il l'a finie) ; négatifs s'ils sont inconnus (GPU sans heures, ou pas encore mesurés).
    struct RenderTimings
    {
        double prepare = 0.0;  // CPU : matrices, poses, lumières, cascades, envoi des données de l'image
        // CPU, parties de `prepare` : exposition, ciel et environnement ; entités (positions, champ, matrices,
        // lumières) ; sol des planètes (parcours des tuiles) ; arbres (plaines) ; envois et lots. Le reste (flammes,
        // traçantes, compteurs) est la différence.
        double prepareSky = 0.0;
        double prepareEntities = 0.0;
        double prepareGround = 0.0;
        double prepareTrees = 0.0;
        double prepareUploads = 0.0;
        double record = 0.0;   // CPU : enregistrement des commandes de dessin et soumission
        double waitGpu = 0.0;  // CPU arrêté : le GPU n'a pas fini l'image qui utilisait les mêmes ressources
        double waitImage = 0.0; // CPU arrêté : aucune image de la fenêtre libre (V-Sync, plafond du pilote)
        double present = 0.0;  // CPU : appel de présentation
        std::uint32_t recordLists = 1; // listes de commandes enregistrées en même temps (SetParallelRecording)
        // GPU : chaque passe du graphe de rendu dans l'ordre de l'image, suivie de ses étapes (step) quand elle en a
        // plusieurs. Rempli quand le panneau Moteur demande les heures (RequestDetailedTimings) ; valable jusqu'à
        // l'image suivante.
        std::span<const RenderPassTiming> gpu;
        std::uint32_t planetTiles = 0;       // tuiles de planète dessinées
        std::uint32_t planetOceanTiles = 0;  // dont redessinées en eau
        std::uint32_t planetStoredTiles = 0; // gardées en mémoire vidéo
        std::uint32_t planetTileCapacity = 0; // au plus, en mémoire vidéo
        std::uint32_t planetComputedTiles = 0; // calculées par le GPU à cette image
        // Threads par groupe des mesh shaders du sol, 0 : par les sommets (choisi au démarrage selon le GPU).
        std::uint32_t groundMeshThreads = 0;
        // Tuiles du sol où pousse l'herbe à cette image, et son chemin (mesh shaders, sinon sommets).
        std::uint32_t grassTiles = 0;
        bool grassMeshShaders = false;
        // Arbres des planètes deux images plus tôt : dessinés (détaillés, simplifiés, lointains) et dans les ombres ;
        // plaines triées à la dernière image, et en vue pas encore semées.
        std::array<std::uint32_t, 3> trees{};
        std::uint32_t shadowTrees = 0;
        // Tuiles et triangles dessinés dans les cartes du relief (près : 3 km, loin : 40 km) à cette image, 0 si gardée.
        std::array<std::uint32_t, 2> terrainShadowTiles{};
        std::array<std::uint32_t, 2> terrainShadowTriangles{};
        std::uint32_t treePlains = 0;
        std::uint32_t pendingTreePlains = 0;
        std::uint32_t aheadTreePlains = 0; // hors du champ, à semer d'avance, pas encore semées
        std::uint32_t sownTreePlains = 0; // semées à cette image
        std::uint32_t hiddenTrees = 0;    // en vue mais cachés derrière le relief ou d'autres objets, pas dessinés
        // Chargement encore en cours (écran de chargement) : tuiles demandées pas encore prêtes, textures du sol pas
        // encore décodées, modèles pas encore lus ou envoyés au GPU, polices dont l'atlas se construit.
        std::uint32_t planetPendingTiles = 0;
        bool groundTexturesLoading = false;
        std::uint32_t modelsLoading = 0;
        std::uint32_t fontsLoading = 0;
        // Exposition de l'image (1 : aucune correction) et luminance moyenne mesurée deux images plus tôt (0 : exposition
        // fixe, ou image noire).
        float exposure = 1.0f;
        float sceneLuminance = 0.0f;
        // Entités (et blocs de grands modèles) de la passe principale testées par le GPU, et dont cachées derrière
        // d'autres (non dessinées), deux images plus tôt.
        std::uint32_t occlusionTested = 0;
        std::uint32_t occlusionHidden = 0;
        // Paquets de triangles des modèles fixes envoyés aux mesh shaders, et dont dessinés (les autres écartés par le
        // GPU : hors champ, de dos, cachés), deux images plus tôt. modelMeshShaders à false : par les sommets.
        std::uint32_t modelMeshlets = 0;
        std::uint32_t modelMeshletsDrawn = 0;
        bool modelMeshShaders = false;
        // Ray tracing (GPU qui l'a) : instances de la structure de la scène, modèles différents qu'elle désigne, et
        // dont construits à cette image ou reportés (entités) ; mémoire vidéo des structures des modèles en vue, puis
        // celle des structures de la scène et de leur mémoire de travail.
        bool rayTracing = false;
        std::uint32_t rayInstances = 0;
        std::uint32_t rayModels = 0;
        std::uint32_t rayModelsBuilt = 0;
        std::uint32_t rayModelsWaiting = 0;
        std::uint64_t rayModelBytes = 0;
        std::uint64_t raySceneBytes = 0;
    };

    // Aucun type d'API graphique ici : le rendu passe par le RHI (src/Renderer/RHI), implémenté avec Vulkan.
    class FLUX_API Renderer {
        public:
        static std::expected<std::unique_ptr<Renderer>, std::string> Create(Window& window);

        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        // Une image : BeginFrame, Render puis EndFrame, appelés à la suite après les mises à jour de la scène (BeginFrame
        // attend le GPU : plus tôt, le CPU attendrait au lieu de travailler). Chacun retourne false sur une erreur
        // irrécupérable (device perdu, etc.). Fenêtre réduite ou swapchain recréée : l'image est sautée, Render et
        // EndFrame ne font rien, mais EndFrame vide quand même l'overlay.
        bool BeginFrame();
        // `hidden` et ses descendants ne sont pas dessinés : le personnage quand la caméra est dans sa tête.
        bool Render(const Scene& scene, const Camera& camera, Entity hidden = NullEntity);
        bool EndFrame();

        void SetClearColor(Math::LinearColor color);

        // Partie de la fenêtre où la scène est dessinée (en pixels, la vue 3D de l'éditeur) ; l'image garde les
        // proportions de ce rectangle. min == max (par défaut) : toute la fenêtre. L'overlay 2D couvre toujours tout.
        void SetSceneViewport(Math::Vector2f min, Math::Vector2f max);
        // false : ni ombres, ni scène 3D, ni ciel ; seule l'interface est dessinée (l'éditeur d'interface couvre la vue).
        void SetSceneVisible(bool visible);
        // Comme SetSceneVisible(false), mais demandé par le jeu (menu opaque) : les deux se cumulent.
        void SetSceneCovered(bool covered);
        // Flammes des réacteurs montrées à pleine poussée, quels que soient les moteurs : dans l'éditeur, jeu arrêté,
        // pour les voir et les régler (sinon, au ralenti, elles sont à peine visibles).
        void SetThrusterPreview(bool preview);
        // Projectiles à dessiner en traçantes, et éclats d'impact, aux prochains Render (Projectiles). Les tableaux
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
        // Mise à l'échelle (DefaultUpscaling par défaut). Un mode FSR sans FSR disponible (DLL d'AMD absente, GPU
        // refusé) donne Natif : GetUpscaling rend le réglage demandé, IsUpscalingAvailable dit si FSR tourne. En FSR,
        // SetTemporalAA ne change rien : FSR fait aussi l'anticrénelage.
        void SetUpscaling(Upscaling upscaling);
        Upscaling GetUpscaling() const;
        bool IsUpscalingAvailable() const;
        // Netteté ajoutée par FSR (0 à 1, DefaultSharpness par défaut).
        void SetSharpness(float sharpness);
        float GetSharpness() const;
        // Taille à laquelle la scène est dessinée à la dernière image (plus petite que la vue en FSR).
        Math::Vector2i GetRenderSize() const;
        // Entités cachées derrière d'autres écartées par le GPU (activé par défaut), d'après la profondeur de l'image
        // précédente. Désactivable pour comparer.
        void SetOcclusionCulling(bool enabled);
        // Fils qui enregistrent les passes de l'image en parallèle (ceux du moteur, qui vivent plus longtemps que le
        // Renderer) ; nul (par défaut) : tout sur le fil de Render.
        void SetJobSystem(JobSystem* jobs);
        // Préparation (entités, traçantes) et enregistrement en parallèle, avec des fils (permis par défaut) :
        // désactivable pour comparer. Permis, les fils ne sont pris que si le CPU limite l'image ; quand il attend le
        // GPU ou l'écran, un seul fil (IsUsingWorkers dit lequel des deux à la dernière image).
        void SetParallelRecording(bool enabled);
        bool IsParallelRecording() const;
        bool IsUsingWorkers() const;
        // Grille au sol de l'éditeur (plan y = 0, lignes du mètre, des 10 m et des 100 m, axes X et Z en couleur),
        // cachée derrière les objets ; désactivée par défaut, le jeu ne la montre pas.
        void SetGridVisible(bool visible);
        bool IsGridVisible() const;
        bool IsOcclusionCullingEnabled() const;
        // Mesure (désactivée par défaut) : le sol des planètes dessiné sans le calcul de ses pixels, gris. L'écart de
        // « dont sol » dans le panneau Moteur donne le coût des pixels ; le reste, celui des sommets et des triangles.
        void SetGroundGeometryOnly(bool enabled);
        bool IsGroundGeometryOnly() const;
        // Vue de débogage des ombres du soleil : chaque pixel teinté selon la carte qu'il lit (rouge, vert, bleu, jaune
        // pour les 4 cascades proches ; magenta et cyan pour le relief au loin ; gris sans ombre).
        void SetShowShadowCascades(bool enabled);
        bool IsShowingShadowCascades() const;
        // Ombres du soleil des modèles fixes en rayons (GPU avec ray tracing, par défaut) ; false : dans les cartes
        // d'ombre, pour comparer. Sans ray tracing, toujours dans les cartes.
        void SetRayTracedShadows(bool enabled);
        bool IsRayTracingShadows() const;
        // Vue de débogage des ombres en rayons (RayShadowRenderer::SetShowMask) ; sans effet sans ray tracing.
        void SetShowRayShadowMask(bool enabled);
        bool IsShowingRayShadowMask() const;
        // Détail du sol des planètes (DefaultGroundDetail par défaut) : plus fin, plus de tuiles à calculer et dessiner.
        void SetGroundDetail(GroundDetail detail);
        GroundDetail GetGroundDetail() const;
        // Qualité des nuages volumétriques (DefaultCloudQuality par défaut) : moins de pas, moins de calcul.
        void SetCloudQuality(CloudQuality quality);
        CloudQuality GetCloudQuality() const;
        // Distance d'affichage des arbres (DefaultVegetationDistance par défaut) : facteur de la portée de chaque planète.
        void SetVegetationDistance(VegetationDistance distance);
        VegetationDistance GetVegetationDistance() const;
        // Mode de présentation réellement utilisé, lisible (FIFO, MAILBOX...).
        std::string_view GetPresentModeName() const;

        // Overlay 2D de la frame en cours, dessiné par-dessus la scène puis vidé par chaque EndFrame.
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

        // Capture d'écran de toute la fenêtre (interface comprise), telle que la prochaine image la présente : prête
        // deux ou trois images plus tard dans TakeCapture (une seule fois). false si la fenêtre ne permet pas la copie.
        bool RequestCapture();
        std::optional<ImageData> TakeCapture();

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
