#pragma once

// RHI (Render Hardware Interface) : ce que le renderer demande au GPU, sans aucun type d'une API graphique. Vulkan
// l'implémente (Renderer/Vulkan) ; un autre backend (DX12) n'aurait qu'à implémenter ces interfaces.
//
// Portée actuelle, celle dont le renderer a besoin : une fenêtre (sa swapchain est tenue par le Device), des
// buffers, des textures 2D, cube et tableaux, des groupes de ressources (textures, buffers de stockage) liés aux shaders, des pipelines
// graphiques et de calcul (compute), et une liste de commandes par image. Une passe dessine dans l'image de la swapchain
// ou dans une texture.
//
// Durées de vie : un objet créé par le Device doit être détruit avant lui ; un BindGroup avant son BindGroupLayout.
// Un objet ne se détruit ni ne se modifie pendant que le GPU l'utilise : après Device::WaitIdle, ou (ressources d'une
// image en vol) une fois BeginFrame revenu sur cette image.

#include "Flux/Math/Color.hpp"
#include "Flux/Export.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace Flux::RHI {
    // Deux images en vol : le CPU prépare la suivante pendant que le GPU dessine la courante.
    inline constexpr std::uint32_t FramesInFlight = 2;

    struct Extent
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;

        bool operator==(const Extent&) const = default;
    };

    struct Rect
    {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    struct Viewport
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float minDepth = 0.0f;
        float maxDepth = 1.0f;
    };

    // Formats des textures, des cibles de rendu et des attributs de sommets.
    enum class Format : std::uint8_t {
        Undefined,
        RGBA8Unorm,
        RGBA8Srgb, // couleurs d'une image : ramenées en linéaire à la lecture
        BGRA8Unorm,
        BGRA8Srgb,
        D32Float, // profondeur 32 bits flottante : le reverse-Z tient l'échelle galaxie
        RG32Float,
        RGB32Float,
        RGBA32Float,
        RGBA16Uint,
        RGBA16Float, // couleur au-delà de 1 (lumières, ciel) sans perte
        RG16Float,   // deux valeurs signées (déplacement des pixels d'une image à l'autre)
    };

    enum class BufferUsage : std::uint8_t {
        Vertex = 1 << 0,
        Index = 1 << 1,   // indices 32 bits
        Storage = 1 << 2, // lu par les shaders (matrices des os), écrit par les compute shaders
        Indirect = 1 << 3, // commandes de dessin lues par le GPU (CommandList::DrawIndexedIndirect)
    };

    enum class ShaderStage : std::uint8_t {
        // Étape de la géométrie : les sommets, ou les étapes de tâche et de maillage qui la remplacent dans un
        // pipeline à mesh shaders (Device::HasMeshShaders) ; une ressource vue des sommets l'est aussi d'elles.
        Vertex = 1 << 0,
        Fragment = 1 << 1,
        Compute = 1 << 2,
    };

    // Opérateurs de bits pour les énumérations de drapeaux.
    template <typename E>
    inline constexpr bool IsFlagEnum = false;
    template <>
    inline constexpr bool IsFlagEnum<BufferUsage> = true;
    template <>
    inline constexpr bool IsFlagEnum<ShaderStage> = true;

    template <typename E>
        requires IsFlagEnum<E>
    constexpr E operator|(E a, E b) {
        return static_cast<E>(static_cast<std::underlying_type_t<E>>(a) | static_cast<std::underlying_type_t<E>>(b));
    }

    template <typename E>
        requires IsFlagEnum<E>
    constexpr bool HasFlag(E flags, E flag) {
        return (static_cast<std::underlying_type_t<E>>(flags) & static_cast<std::underlying_type_t<E>>(flag)) != 0;
    }

    enum class MemoryType : std::uint8_t {
        GpuOnly, // mémoire vidéo, remplie une fois à la création
        Upload,  // visible du CPU, écrite à chaque image (Buffer::Write)
        Readback, // écrite par le GPU, relue par le CPU (Buffer::Read) une fois le GPU passé
    };

    struct BufferDesc
    {
        std::uint64_t size = 0;
        BufferUsage usage = BufferUsage::Vertex;
        MemoryType memory = MemoryType::GpuOnly;
    };

    // Une commande de CommandList::DrawIndexedIndirect, comme VkDrawIndexedIndirectCommand. firstInstance arrive
    // au vertex shader (SV_VulkanInstanceID) : les shaders de Mesh.slang y lisent l'indice de leurs données.
    struct DrawIndexedCommand
    {
        std::uint32_t indexCount;
        std::uint32_t instanceCount;
        std::uint32_t firstIndex;
        std::int32_t vertexOffset;
        std::uint32_t firstInstance;
    };

    enum class Filter : std::uint8_t { Nearest, Linear };
    enum class AddressMode : std::uint8_t { Repeat, ClampToEdge };

    struct SamplerDesc
    {
        Filter filter = Filter::Linear;
        Filter mipFilter = Filter::Linear;
        AddressMode address = AddressMode::Repeat;
        // Carte d'ombres : la lecture compare une profondeur donnée à celle de la texture (1 si elle est plus proche
        // ou égale, 0 sinon) ; filtrée, la comparaison adoucit le bord des ombres.
        bool compare = false;
        // Filtrage anisotrope (1 : aucun) : une texture vue de biais (le sol au loin) reste nette ; borné par le GPU.
        float anisotropy = 1.0f;
    };

    // Une ressource d'un groupe : texture échantillonnée (avec son sampler), buffer de stockage (lu par les shaders de
    // dessin, lu et écrit par les compute shaders), ou table de textures (`count` cases, `Sampler2D t[]` dans les
    // shaders) : toutes les images des matériaux dans un seul groupe, choisies par indice dans le shader. Les cases non
    // remplies ne doivent pas être lues ; une case que le GPU ne lit pas peut être remplie pendant qu'il dessine.
    enum class BindingType : std::uint8_t { Texture, StorageBuffer, TextureTable };

    struct Binding
    {
        std::uint32_t slot = 0;
        BindingType type = BindingType::Texture;
        ShaderStage stages = ShaderStage::Fragment;
        std::uint32_t count = 1; // TextureTable seulement : nombre de cases
    };

    struct BindGroupLayoutDesc
    {
        std::span<const Binding> bindings;
        // Groupes de ce type vivants en même temps, au plus (réservés d'avance) ; au-delà, CreateBindGroup échoue.
        std::uint32_t maxGroups = 1;
    };

    struct VertexBufferLayout
    {
        std::uint32_t binding = 0;
        std::uint32_t stride = 0;
    };

    struct VertexAttribute
    {
        std::uint32_t location = 0;
        std::uint32_t binding = 0;
        Format format = Format::Undefined;
        std::uint32_t offset = 0;
    };

    enum class CullMode : std::uint8_t { None, Back, Front }; // faces avant : sens antihoraire
    enum class CompareOp : std::uint8_t { Greater, GreaterOrEqual, Less, LessOrEqual, Always };
    enum class BlendMode : std::uint8_t {
        Opaque,
        Alpha, // mélange classique, couleur non prémultipliée (interface, texte)
        Additive, // couleur ajoutée à ce qui est derrière (lumière : flammes, lueurs), dans n'importe quel ordre
    };

    class BindGroupLayout;

    struct PipelineDesc
    {
        // Programme compilé (SPIR-V pour Vulkan) contenant les deux points d'entrée.
        std::filesystem::path shaderPath;
        const char* vertexEntryPoint = "vertexMain";
        const char* fragmentEntryPoint = "fragmentMain"; // nullptr : aucun (passe de profondeur seule)
        // Pipeline à mesh shaders (Device::HasMeshShaders) : étape de maillage, et de tâche avant elle, à la place
        // des sommets (vertexEntryPoint, vertexBuffers et vertexAttributes ignorés) ; dessiné par DrawMeshTasks.
        const char* taskEntryPoint = nullptr;
        const char* meshEntryPoint = nullptr;
        std::span<const VertexBufferLayout> vertexBuffers;
        std::span<const VertexAttribute> vertexAttributes;
        // Groupe i : layouts[i] (BindGroup lié à l'indice i).
        std::span<const BindGroupLayout* const> bindGroupLayouts;
        // Petites constantes envoyées avec chaque appel de dessin (CommandList::PushConstants), vues des deux
        // étapes ; 128 octets au plus sont garantis partout.
        std::uint32_t pushConstantsSize = 0;
        Format colorFormat = Format::Undefined; // Undefined : pas de couleur (passe de profondeur seule)
        // Seconde cible de couleur de la passe (RenderPassDesc::secondColorTarget), écrite par SV_Target1 si
        // writeSecondColor ; sinon laissée telle quelle (le shader n'a pas à l'écrire).
        Format secondColorFormat = Format::Undefined;
        bool writeSecondColor = false;
        // false : la couleur n'est pas écrite, seulement la profondeur (pré-passe de profondeur dans une passe avec
        // couleur).
        bool writeColor = true;
        // Doit correspondre à la passe, même si ce pipeline ne teste pas la profondeur.
        Format depthFormat = Format::Undefined;
        bool depthTest = false;
        bool depthWrite = false;
        CompareOp depthCompare = CompareOp::Greater; // reverse-Z : plus proche = plus grand
        CullMode cullMode = CullMode::None;
        BlendMode blend = BlendMode::Opaque;
        // Décalage de la profondeur écrite (carte d'ombres) : constant, et selon la pente de la face.
        float depthBiasConstant = 0.0f;
        float depthBiasSlope = 0.0f;
    };

    // Pipeline de calcul : un point d'entrée [shader("compute")] lancé par CommandList::Dispatch.
    struct ComputePipelineDesc
    {
        std::filesystem::path shaderPath;
        const char* entryPoint = "computeMain";
        std::span<const BindGroupLayout* const> bindGroupLayouts;
        std::uint32_t pushConstantsSize = 0; // 128 octets au plus
    };

    class Buffer {
        public:
        virtual ~Buffer() = default;
        virtual std::uint64_t GetSize() const = 0;
        // Buffer Upload seulement.
        virtual void Write(const void* data, std::uint64_t size, std::uint64_t offset = 0) = 0;
        // Buffer Readback seulement, quand le GPU a fini de l'écrire (après Device::Submit, ou BeginFrame revenu sur
        // l'image qui l'a écrit).
        virtual void Read(void* data, std::uint64_t size, std::uint64_t offset = 0) const = 0;
    };

    class Texture {
        public:
        virtual ~Texture() = default;
        virtual Extent GetExtent() const = 0;
        virtual Format GetFormat() const = 0;
    };

    // Ce qu'une passe fait d'une texture : le graphe de rendu le déclare (RenderGraph), CommandList::UseTextures en
    // déduit les barrières. Une texture remplie par envoi (CreateTexture…) commence en Sampled, une cible en None.
    enum class TextureAccess : std::uint8_t {
        None,        // jamais utilisée : contenu indéfini
        ColorTarget, // cible de couleur d'une passe de dessin
        DepthTarget, // tampon de profondeur d'une passe de dessin
        Sampled,     // lue par les shaders (sommets, pixels, calcul)
    };

    // `discard` : le contenu d'avant ne sert plus (la passe efface tout ou réécrit tout), le GPU peut le jeter.
    struct TextureUse
    {
        const Texture* texture = nullptr;
        TextureAccess access = TextureAccess::None;
        bool discard = false;
    };

    class Sampler {
        public:
        virtual ~Sampler() = default;
    };

    class BindGroupLayout {
        public:
        virtual ~BindGroupLayout() = default;
    };

    // Ressources liées ensemble aux shaders (un « descriptor set » Vulkan).
    class BindGroup {
        public:
        virtual ~BindGroup() = default;
        virtual void SetTexture(std::uint32_t slot, const Texture& texture, const Sampler& sampler) = 0;
        virtual void SetBuffer(std::uint32_t slot, const Buffer& buffer) = 0;
        // Case `element` d'une table de textures (BindingType::TextureTable).
        virtual void SetTableTexture(std::uint32_t slot, std::uint32_t element, const Texture& texture, const Sampler& sampler) = 0;
    };

    class Pipeline {
        public:
        virtual ~Pipeline() = default;
    };

    // Passe qui dessine dans l'image de la swapchain, effacée, avec un tampon de profondeur effacé (s'il y en a un).
    // Sans couleur (color = false) : profondeur seule dans `depth` (carte d'ombres, CreateRenderTarget sampled).
    // Avec colorTarget : dans une texture (CreateRenderTarget sampled, de couleur) ou dans la face colorLayer d'une
    // texture cube (CreateCubeRenderTarget), avec ou sans profondeur.
    // Les textures doivent déjà servir de cibles (UseTextures : ColorTarget, DepthTarget) ; la passe ne change pas leur
    // usage : pour les lire ensuite, UseTextures(Sampled). Seule l'image de la swapchain est préparée par la passe.
    struct RenderPassDesc
    {
        Math::LinearColor clearColor;
        const Texture* depth = nullptr; // Format::D32Float, de la taille de la cible (swapchain, texture, carte d'ombres)
        float clearDepth = 0.0f;        // reverse-Z : 0 = infiniment loin
        bool color = true;
        const Texture* colorTarget = nullptr;
        std::uint32_t colorLayer = 0;
        // Avec colorTarget seulement : seconde texture de la même taille, effacée à 0.
        const Texture* secondColorTarget = nullptr;
        // Tampon de profondeur gardé après la passe, pour être lu ensuite (CreateRenderTarget sampled).
        bool keepDepth = false;
        // Sans couleur seulement : la profondeur d'avant est gardée au lieu d'être effacée (une passe précédente l'a
        // déjà remplie) ; CommandList::ClearDepth efface les parties à redessiner.
        bool loadDepth = false;
    };

    // Commandes d'une image, enregistrées entre Device::BeginFrame et Device::EndFrame.
    class CommandList {
        public:
        virtual ~CommandList() = default;
        // Prépare chaque texture à son nouvel usage, en une barrière pour toutes : attend ce qui l'écrivait ou la
        // lisait avant, change son layout. Sans effet pour une texture déjà lue (Sampled) et relue. Hors d'une passe.
        virtual void UseTextures(std::span<const TextureUse> uses) = 0;
        void Use(const Texture& texture, TextureAccess access, bool discard = false) {
            const TextureUse use{&texture, access, discard};
            UseTextures({&use, 1});
        }
        virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;
        virtual void EndRenderPass() = 0;
        virtual void SetViewport(const Viewport& viewport) = 0;
        virtual void SetScissor(const Rect& rect) = 0;
        // Dans une passe : remplit `rect` de la profondeur `depth` (passe à profondeur gardée, RenderPassDesc::loadDepth).
        virtual void ClearDepth(const Rect& rect, float depth) = 0;
        virtual void BindPipeline(const Pipeline& pipeline) = 0;
        // Pipeline lié avant : ses layouts disent où vont les groupes. Un groupe reste lié d'un pipeline à un autre
        // si les deux déclarent les mêmes layouts jusqu'à cet indice.
        virtual void SetBindGroup(std::uint32_t index, const BindGroup& group) = 0;
        virtual void BindVertexBuffers(std::uint32_t first, std::span<const Buffer* const> buffers) = 0;
        virtual void BindIndexBuffer(const Buffer& buffer) = 0;
        virtual void PushConstants(const void* data, std::uint32_t size) = 0;
        virtual void Draw(std::uint32_t vertexCount, std::uint32_t firstVertex) = 0;
        // Une instance, d'indice `instance` (SV_VulkanInstanceID).
        virtual void DrawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex, std::uint32_t instance = 0) = 0;
        // `count` commandes DrawIndexedCommand lues dans `buffer` (BufferUsage::Indirect) à partir de `offset` : un
        // seul appel pour beaucoup de maillages qui partagent pipeline, sommets et indices.
        virtual void DrawIndexedIndirect(const Buffer& buffer, std::uint64_t offset, std::uint32_t count) = 0;
        // Pipeline à mesh shaders lié : `groups` groupes de l'étape de tâche (SV_GroupID.x de 0 à groups - 1).
        virtual void DrawMeshTasks(std::uint32_t groups) = 0;
        // Compute shader du pipeline lié (CreateComputePipeline), en groupes de threads ; hors d'une passe.
        virtual void Dispatch(std::uint32_t groupsX, std::uint32_t groupsY = 1, std::uint32_t groupsZ = 1) = 0;
        // Les buffers écrits par les Dispatch précédents deviennent lisibles par la suite : sommets, index, shaders,
        // et CPU (Readback). Hors d'une passe.
        virtual void ComputeBarrier() = 0;
        // Note l'heure du GPU quand tout ce qui précède est fini, dans la case `slot` (< MaxTimestamps) de l'image en
        // cours ; relue deux images plus tard par Device::GetTimestamps. Sans effet si le GPU ne sait pas le faire.
        virtual void WriteTimestamp(std::uint32_t slot) = 0;
        // Diagnostic de plantage : les commandes qui suivent forment l'étape `name` (texte constant, vivant jusqu'à la
        // fin du programme), jusqu'à l'étape suivante ou la fin de l'image ; MaxSteps par image au plus. Si le GPU se
        // bloque, le journal et le fichier plantage-gpu.txt disent quelles étapes avaient commencé ou fini. Sans effet
        // sans l'extension qui le permet (AMD) ni hors d'une image.
        virtual void BeginStep(const char* name) = 0;
    };

    inline constexpr std::uint32_t MaxTimestamps = 32;
    inline constexpr std::uint32_t MaxSteps = 32;

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

    enum class PresentMode : std::uint8_t { Fifo, FifoRelaxed, Mailbox, Immediate, Other };

    enum class FrameStatus : std::uint8_t {
        Ready, // la liste de commandes attend
        Skip,  // rien à dessiner cette fois (swapchain recréée) : réessayer à l'image suivante
        Error, // erreur irrécupérable (device perdu, etc.)
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
        // Doubles permis dans les shaders (shaderFloat64) : facultatif, les tuiles de planète s'en servent quand il y est.
        virtual bool HasFloat64() const = 0;
        // Mesh shaders (étapes de tâche et de maillage) : facultatifs, le sol des planètes s'en sert quand ils y sont.
        virtual bool HasMeshShaders() const = 0;
        // Attend que le GPU ait tout fini (rechargement d'une ressource en service, fermeture).
        virtual void WaitIdle() = 0;

        // Envois vers la mémoire vidéo (CreateBuffer avec données, WriteBuffer d'un buffer GpuOnly, textures) : avec une
        // fenêtre, soumis avec la prochaine image (avant ses commandes, après tout ce qui a été soumis avant) sans
        // attendre le GPU ; sans fenêtre, attendus tout de suite. Une ressource envoyée doit vivre au moins jusqu'à la
        // fin de la prochaine image.
        // GpuOnly : `data` (size octets) y est copié. Upload : `data` peut être nul.
        virtual std::expected<std::unique_ptr<Buffer>, std::string> CreateBuffer(const BufferDesc& desc, const void* data = nullptr) = 0;
        // Remplit une partie d'un buffer : GpuOnly par une copie (voir plus haut), Upload directement. false si la copie
        // ne peut être préparée.
        virtual bool WriteBuffer(Buffer& buffer, std::uint64_t offset, const void* data, std::uint64_t size) = 0;
        // Texture échantillonnée remplie de pixels (4 octets chacun) ; mipmaps : réductions successives calculées
        // par le GPU, pour une texture vue de loin sans scintillement.
        virtual std::expected<std::unique_ptr<Texture>, std::string> CreateTexture(Format format, Extent extent,
                                                                                   std::span<const std::uint8_t> pixels,
                                                                                   bool mipmaps = false) = 0;
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
        // Cible de rendu vierge : tampon de profondeur, ou couleur (RenderPassDesc::colorTarget). sampled : lue
        // ensuite par les shaders (carte d'ombres, image de la scène avant le bloom).
        virtual std::expected<std::unique_ptr<Texture>, std::string> CreateRenderTarget(Format format, Extent extent,
                                                                                        bool sampled = false) = 0;
        // Texture cube vierge dont chaque face est une cible de rendu (RenderPassDesc::colorTarget), lue ensuite par
        // direction comme CreateCubeTexture. Un seul niveau de mip.
        virtual std::expected<std::unique_ptr<Texture>, std::string> CreateCubeRenderTarget(Format format, std::uint32_t size) = 0;
        virtual std::expected<std::unique_ptr<Sampler>, std::string> CreateSampler(const SamplerDesc& desc) = 0;
        virtual std::expected<std::unique_ptr<BindGroupLayout>, std::string> CreateBindGroupLayout(const BindGroupLayoutDesc& desc) = 0;
        // nullptr si le layout a atteint son maximum de groupes.
        virtual std::unique_ptr<BindGroup> CreateBindGroup(const BindGroupLayout& layout) = 0;
        virtual std::expected<std::unique_ptr<Pipeline>, std::string> CreatePipeline(const PipelineDesc& desc) = 0;
        virtual std::expected<std::unique_ptr<Pipeline>, std::string> CreateComputePipeline(const ComputePipelineDesc& desc) = 0;
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
    };
}
