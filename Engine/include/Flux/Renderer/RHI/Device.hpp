#pragma once

// Device du RHI : création des ressources, images en vol, envois.

#include "Flux/Assets/Image.hpp"
#include "Flux/Export.hpp"
#include "Flux/Renderer/RHI/BindGroup.hpp"
#include "Flux/Renderer/RHI/Buffer.hpp"
#include "Flux/Renderer/RHI/CommandList.hpp"
#include "Flux/Renderer/RHI/Pipeline.hpp"
#include "Flux/Renderer/RHI/Texture.hpp"
#include "Flux/Renderer/RHI/Upscaler.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Flux::RHI {
    // Temps passés par le CPU à attendre, pendant l'image précédente (millisecondes) : longs, ils disent que le GPU ou
    // la présentation (V-Sync, plafond du pilote) mène la cadence.
    struct WaitTimes
    {
        double frame = 0.0;   // attente que le GPU ait fini l'image qui utilisait les mêmes ressources
        double acquire = 0.0; // attente d'une image libre de la swapchain
        double present = 0.0; // appel de présentation
    };

    // Mémoire réservée auprès du pilote par l'allocateur (octets) : celle du GPU, et celle de la RAM qu'il peut lire.
    struct MemoryUsage
    {
        std::uint64_t device = 0;
        std::uint64_t host = 0;
    };

    // GPU, fenêtre et images en vol. Crée les ressources ; BeginFrame / EndFrame encadrent chaque image.
    class Device {
        public:
        // Backend Vulkan, sur la fenêtre native donnée (HWND).
        FLUX_API static std::expected<std::unique_ptr<Device>, std::string> Create(void* nativeWindowHandle, Extent extent, bool vsync);
        // Sans fenêtre ni swapchain (tests, calculs) : seul Submit envoie des commandes ; BeginFrame échoue.
        FLUX_API static std::expected<std::unique_ptr<Device>, std::string> CreateHeadless();

        virtual ~Device() = default;

        virtual std::string_view GetName() const = 0;
        // Version de l'API graphique utilisée, lisible (« 1.3 (GPU : 1.4.303) » pour Vulkan).
        virtual std::string_view GetApiVersion() const = 0;
        virtual MemoryUsage GetMemoryUsage() const = 0;
        // Mesh shaders (étapes de tâche et de maillage) : facultatifs, le sol des planètes s'en sert quand ils y sont.
        virtual bool HasMeshShaders() const = 0;
        // Nombre de threads par groupe de maillage que le pilote préfère (0 sans mesh shaders).
        virtual std::uint32_t GetMeshPreferredThreads() const = 0;
        // Formats compressés BC (textureCompressionBC) : tous les GPU de bureau, facultatif quand même.
        virtual bool HasTextureCompressionBC() const = 0;
        // Attend que le GPU ait tout fini (fermeture) ; appelle les libérations différées (Retire).
        virtual void WaitIdle() = 0;

        // Envois vers la mémoire vidéo (CreateBuffer avec données, WriteBuffer d'un buffer GpuOnly, textures) : avec une
        // fenêtre, copiés par la file de transfert (la file graphique sans elle) et attendus par la prochaine image, sans
        // attendre le GPU ; sans fenêtre, attendus tout de suite. WriteBuffer attend d'abord la fin des images déjà
        // soumises (elles lisent peut-être ce qui est réécrit). Une ressource envoyée doit vivre au moins jusqu'à la
        // fin de la prochaine image.
        // GpuOnly : `data` (size octets) y est copié. Upload : `data` peut être nul.
        virtual std::expected<std::unique_ptr<Buffer>, std::string> CreateBuffer(const BufferDesc& desc, const void* data = nullptr) = 0;
        // Remplit une partie d'un buffer : GpuOnly par une copie (voir plus haut), Upload directement. false si la copie
        // ne peut être préparée. inUse faux : aucune image soumise ne lit cette plage (place neuve d'un buffer
        // partagé, rendue seulement par Retire), la copie n'attend pas leur fin.
        virtual bool WriteBuffer(Buffer& buffer, std::uint64_t offset, const void* data, std::uint64_t size, bool inUse = true) = 0;
        // Texture échantillonnée remplie de pixels (4 octets chacun) ; mipmaps : réductions successives calculées
        // par le GPU, pour une texture vue de loin sans scintillement.
        virtual std::expected<std::unique_ptr<Texture>, std::string> CreateTexture(Format format, Extent extent,
                                                                                   std::span<const std::uint8_t> pixels,
                                                                                   bool mipmaps = false) = 0;
        // La même depuis une mémoire d'envoi (RGBA8, à `offset`), sans recopie.
        virtual std::expected<std::unique_ptr<Texture>, std::string> CreateTexture(Format format, Extent extent,
                                                                                   const StagingBuffer& source, std::uint64_t offset,
                                                                                   bool mipmaps = false) = 0;
        // Mémoire d'envoi de `size` octets ; appelable depuis n'importe quel fil (fils de chargement).
        virtual std::expected<std::unique_ptr<StagingBuffer>, std::string> CreateStagingBuffer(std::uint64_t size) = 0;
        // Rend une mémoire d'envoi dont les copies sont enregistrées : détruite quand elles sont finies.
        virtual void ReleaseStaging(std::unique_ptr<StagingBuffer> staging) = 0;

        // Envois asynchrones (avec une fenêtre) : ceux faits entre BeginAsyncUploads et EndAsyncUploads ne retardent
        // aucune image. Leurs ressources ne doivent servir qu'une fois IsUploadReady vrai pour le numéro rendu (une
        // image ou deux plus tard) ; sans fenêtre, tout de suite. Pour les gros envois (modèles, textures du sol).
        virtual void BeginAsyncUploads() = 0;
        virtual std::uint64_t EndAsyncUploads() = 0;
        virtual bool IsUploadReady(std::uint64_t ticket) const = 0;
        // Libération différée : `release` (qui détruit une ressource remplacée, rend ses places) est appelée quand le
        // GPU a fini ce qui a été soumis ou enregistré jusqu'ici, au début d'une image suivante, sans arrêter le GPU.
        // Ce qu'elle touche doit vivre jusqu'à son appel (WaitIdle les appelle toutes).
        virtual void Retire(std::move_only_function<void()> release) = 0;

        // Texture cube (ciel, reflets) : 6 faces carrées de `size` pixels, à la suite dans pixels, dans l'ordre +X, -X,
        // +Y, -Y, +Z, -Z ; lue par direction (TextureCube dans les shaders).
        virtual std::expected<std::unique_ptr<Texture>, std::string> CreateCubeTexture(Format format, std::uint32_t size,
                                                                                       std::span<const std::uint8_t> pixels,
                                                                                       bool mipmaps = false) = 0;
        // Tableau de `layers` textures de même taille (sol des planètes : une couche par matière), à la suite dans
        // pixels ; lu par coordonnées et indice de couche (Sampler2DArray dans les shaders).
        virtual std::expected<std::unique_ptr<Texture>, std::string> CreateTextureArray(Format format, Extent extent,
                                                                                        std::uint32_t layers,
                                                                                        std::span<const std::uint8_t> pixels,
                                                                                        bool mipmaps = false) = 0;
        // Le même tableau avec ses `levels` niveaux de mip déjà calculés (formats compressés : le GPU ne sait pas les
        // réduire). pixels : niveau 0 de toutes les couches, puis niveau 1, etc. ; chaque niveau moitié du précédent,
        // arrondi en dessous, jusqu'à 1.
        virtual std::expected<std::unique_ptr<Texture>, std::string> CreateTextureArrayLevels(Format format, Extent extent,
                                                                                              std::uint32_t layers,
                                                                                              std::uint32_t levels,
                                                                                              std::span<const std::uint8_t> pixels) = 0;
        // Texture 3D vierge de extent x depth pixels, remplie par les compute shaders (BindingType::StorageTexture,
        // TextureAccess::Storage) puis lue par position (Sampler3D dans les shaders) : bruit des nuages. mipmaps : de
        // quoi la réduire jusqu'à 1 pixel (CommandList::GenerateMipmaps). Format RGBA8Unorm seulement : le seul que
        // tous les GPU savent écrire ainsi et réduire.
        virtual std::expected<std::unique_ptr<Texture>, std::string> CreateVolumeTexture(Format format, Extent extent,
                                                                                         std::uint32_t depth,
                                                                                         bool mipmaps = false) = 0;
        // Cible de rendu vierge : tampon de profondeur, ou couleur (RenderPassDesc::colorTarget). sampled : lue
        // ensuite par les shaders (carte d'ombres, image de la scène avant le bloom) ; storage : aussi écrite par les
        // compute shaders (TextureAccess::Storage), couleur seulement. mipLevels (couleur seulement) : niveaux de mip,
        // chacun moitié du précédent, dessinés un à un (RenderPassDesc::colorMip) ou réduits par
        // CommandList::GenerateMipmaps.
        virtual std::expected<std::unique_ptr<Texture>, std::string> CreateRenderTarget(Format format, Extent extent,
                                                                                        bool sampled = false,
                                                                                        bool storage = false,
                                                                                        std::uint32_t mipLevels = 1) = 0;
        // Texture cube vierge dont chaque face de chaque niveau de mip est une cible de rendu (RenderPassDesc::colorTarget,
        // colorLayer, colorMip), lue ensuite par direction comme CreateCubeTexture. Plusieurs niveaux : chacun moitié du
        // précédent, dessinés un à un ou réduits par CommandList::GenerateMipmaps.
        virtual std::expected<std::unique_ptr<Texture>, std::string> CreateCubeRenderTarget(Format format, std::uint32_t size,
                                                                                            std::uint32_t mipLevels = 1) = 0;
        virtual std::expected<std::unique_ptr<Sampler>, std::string> CreateSampler(const SamplerDesc& desc) = 0;
        virtual std::expected<std::unique_ptr<BindGroupLayout>, std::string> CreateBindGroupLayout(const BindGroupLayoutDesc& desc) = 0;
        // nullptr si le layout a atteint son maximum de groupes.
        virtual std::unique_ptr<BindGroup> CreateBindGroup(const BindGroupLayout& layout) = 0;
        virtual std::expected<std::unique_ptr<Pipeline>, std::string> CreatePipeline(const PipelineDesc& desc) = 0;
        virtual std::expected<std::unique_ptr<Pipeline>, std::string> CreateComputePipeline(const ComputePipelineDesc& desc) = 0;
        // Agrandisseur temporel pour une vue de `outputSize` (la taille de rendu peut aller jusqu'à elle). Erreur si
        // la bibliothèque d'AMD manque ou si le GPU la refuse : le rendu reste alors en Natif.
        virtual std::expected<std::unique_ptr<Upscaler>, std::string> CreateUpscaler(Extent outputSize) = 0;
        // Enregistre des commandes hors des images (calculs, sans passe vers la swapchain), les envoie et attend que le
        // GPU les ait finies : bloque le CPU, réservé au chargement et aux tests.
        virtual bool Submit(const std::function<void(CommandList&)>& record) = 0;

        // Taille de la fenêtre et V-Sync : la swapchain est recréée si l'une change (le GPU attendu avant).
        virtual bool ResizeSwapchain(Extent extent, bool vsync) = 0;
        virtual Extent GetSwapchainExtent() const = 0;
        virtual Format GetSwapchainFormat() const = 0;
        // Mode réellement obtenu, qui peut différer du mode demandé.
        virtual PresentMode GetPresentMode() const = 0;

        // Attend que le GPU ait fini l'image précédente de même indice (ses ressources sont de nouveau libres), prend
        // une image de la swapchain et ouvre la liste de commandes.
        virtual FrameStatus BeginFrame() = 0;
        virtual CommandList& GetCommandList() = 0;
        // 0 à FramesInFlight - 1 : l'image en préparation, pour choisir ses ressources propres.
        virtual std::uint32_t GetFrameIndex() const = 0;
        // Envoie les commandes et présente l'image ; false sur une erreur irrécupérable.
        virtual bool EndFrame() = 0;

        // Heures notées par WriteTimestamp, relues à BeginFrame pour la dernière image qui a fini dans ces ressources :
        // millisecondes depuis la case 0 ; négatif pour une case non écrite. Vide si le GPU ne note pas l'heure.
        virtual std::span<const double> GetTimestamps() const = 0;
        virtual const WaitTimes& GetWaitTimes() const = 0;

        // Capture d'écran : l'image de la fenêtre de la prochaine image présentée (interface comprise), copiée pour le
        // CPU. false si la fenêtre ne permet pas la copie.
        virtual bool RequestCapture() = 0;
        // La capture demandée, une fois que le GPU a fini cette image (deux ou trois images plus tard), rendue une
        // seule fois ; nullopt avant.
        virtual std::optional<ImageData> TakeCapture() = 0;
    };
}
