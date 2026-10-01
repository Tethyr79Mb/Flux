#pragma once

// RHI (Render Hardware Interface) : ce que le renderer demande au GPU, sans aucun type d'une API graphique. Vulkan
// l'implémente (Renderer/Vulkan) ; un autre backend (DX12) n'aurait qu'à implémenter ces interfaces.
//
// Portée actuelle, celle dont le renderer a besoin : une fenêtre (sa swapchain est tenue par le Device), des
// buffers, des textures 2D, des groupes de ressources (textures, buffers de stockage) liés aux shaders, des pipelines
// graphiques et une liste de commandes par image. Une passe dessine dans l'image de la swapchain.
//
// Durées de vie : un objet créé par le Device doit être détruit avant lui ; un BindGroup avant son BindGroupLayout.
// Un objet ne se détruit ni ne se modifie pendant que le GPU l'utilise : après Device::WaitIdle, ou (ressources d'une
// image en vol) une fois BeginFrame revenu sur cette image.

#include "Flux/Math/Color.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
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
    };

    enum class BufferUsage : std::uint8_t {
        Vertex = 1 << 0,
        Index = 1 << 1,   // indices 32 bits
        Storage = 1 << 2, // lu par les shaders (matrices des os)
    };

    enum class ShaderStage : std::uint8_t {
        Vertex = 1 << 0,
        Fragment = 1 << 1,
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
    };

    struct BufferDesc
    {
        std::uint64_t size = 0;
        BufferUsage usage = BufferUsage::Vertex;
        MemoryType memory = MemoryType::GpuOnly;
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
    };

    // Une ressource d'un groupe : texture échantillonnée (avec son sampler) ou buffer de stockage en lecture.
    enum class BindingType : std::uint8_t { Texture, StorageBuffer };

    struct Binding
    {
        std::uint32_t slot = 0;
        BindingType type = BindingType::Texture;
        ShaderStage stages = ShaderStage::Fragment;
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
    };

    class BindGroupLayout;

    struct PipelineDesc
    {
        // Programme compilé (SPIR-V pour Vulkan) contenant les deux points d'entrée.
        std::filesystem::path shaderPath;
        const char* vertexEntryPoint = "vertexMain";
        const char* fragmentEntryPoint = "fragmentMain";
        std::span<const VertexBufferLayout> vertexBuffers;
        std::span<const VertexAttribute> vertexAttributes;
        // Groupe i : layouts[i] (BindGroup lié à l'indice i).
        std::span<const BindGroupLayout* const> bindGroupLayouts;
        // Petites constantes envoyées avec chaque appel de dessin (CommandList::PushConstants), vues des deux
        // étapes ; 128 octets au plus sont garantis partout.
        std::uint32_t pushConstantsSize = 0;
        Format colorFormat = Format::Undefined; // Undefined : pas de couleur (passe de profondeur seule)
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

    class Buffer {
        public:
        virtual ~Buffer() = default;
        virtual std::uint64_t GetSize() const = 0;
        // Buffer Upload seulement.
        virtual void Write(const void* data, std::uint64_t size, std::uint64_t offset = 0) = 0;
    };

    class Texture {
        public:
        virtual ~Texture() = default;
        virtual Extent GetExtent() const = 0;
        virtual Format GetFormat() const = 0;
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
    };

    class Pipeline {
        public:
        virtual ~Pipeline() = default;
    };

    // Passe qui dessine dans l'image de la swapchain, effacée, avec un tampon de profondeur effacé (s'il y en a un).
    // Sans couleur (color = false) : profondeur seule dans `depth` (carte d'ombres, CreateRenderTarget sampled),
    // lisible par les shaders après EndRenderPass.
    struct RenderPassDesc
    {
        Math::LinearColor clearColor;
        const Texture* depth = nullptr; // Format::D32Float, de la taille de la swapchain (ou de la carte d'ombres)
        float clearDepth = 0.0f;        // reverse-Z : 0 = infiniment loin
        bool color = true;
    };

    // Commandes d'une image, enregistrées entre Device::BeginFrame et Device::EndFrame.
    class CommandList {
        public:
        virtual ~CommandList() = default;
        virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;
        virtual void EndRenderPass() = 0;
        virtual void SetViewport(const Viewport& viewport) = 0;
        virtual void SetScissor(const Rect& rect) = 0;
        virtual void BindPipeline(const Pipeline& pipeline) = 0;
        // Pipeline lié avant : ses layouts disent où vont les groupes. Un groupe reste lié d'un pipeline à un autre
        // si les deux déclarent les mêmes layouts jusqu'à cet indice.
        virtual void SetBindGroup(std::uint32_t index, const BindGroup& group) = 0;
        virtual void BindVertexBuffers(std::uint32_t first, std::span<const Buffer* const> buffers) = 0;
        virtual void BindIndexBuffer(const Buffer& buffer) = 0;
        virtual void PushConstants(const void* data, std::uint32_t size) = 0;
        virtual void Draw(std::uint32_t vertexCount, std::uint32_t firstVertex) = 0;
        virtual void DrawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex) = 0;
        // Note l'heure du GPU quand tout ce qui précède est fini, dans la case `slot` (< MaxTimestamps) de l'image en
        // cours ; relue deux images plus tard par Device::GetTimestamps. Sans effet si le GPU ne sait pas le faire.
        virtual void WriteTimestamp(std::uint32_t slot) = 0;
    };

    inline constexpr std::uint32_t MaxTimestamps = 8;

    // Temps passés par le CPU à attendre, pendant l'image précédente (millisecondes) : longs, ils disent que le GPU ou
    // la présentation (V-Sync, plafond du pilote) mène la cadence.
    struct WaitTimes
    {
        double frame = 0.0;   // attente que le GPU ait fini l'image qui utilisait les mêmes ressources
        double acquire = 0.0; // attente d'une image libre de la swapchain
        double present = 0.0; // appel de présentation
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
        static std::expected<std::unique_ptr<Device>, std::string> Create(void* nativeWindowHandle, Extent extent, bool vsync);

        virtual ~Device() = default;

        virtual std::string_view GetName() const = 0;
        // Attend que le GPU ait tout fini (rechargement d'une ressource en service, fermeture).
        virtual void WaitIdle() = 0;

        // GpuOnly : `data` (size octets) y est copié, et la copie attendue. Upload : `data` peut être nul.
        virtual std::expected<std::unique_ptr<Buffer>, std::string> CreateBuffer(const BufferDesc& desc, const void* data = nullptr) = 0;
        // Texture échantillonnée remplie de pixels (4 octets chacun) ; mipmaps : réductions successives calculées
        // par le GPU, pour une texture vue de loin sans scintillement.
        virtual std::expected<std::unique_ptr<Texture>, std::string> CreateTexture(Format format, Extent extent,
                                                                                   std::span<const std::uint8_t> pixels,
                                                                                   bool mipmaps = false) = 0;
        // Cible de rendu vierge (tampon de profondeur). sampled : lue ensuite par les shaders (carte d'ombres).
        virtual std::expected<std::unique_ptr<Texture>, std::string> CreateRenderTarget(Format format, Extent extent,
                                                                                        bool sampled = false) = 0;
        virtual std::expected<std::unique_ptr<Sampler>, std::string> CreateSampler(const SamplerDesc& desc) = 0;
        virtual std::expected<std::unique_ptr<BindGroupLayout>, std::string> CreateBindGroupLayout(const BindGroupLayoutDesc& desc) = 0;
        // nullptr si le layout a atteint son maximum de groupes.
        virtual std::unique_ptr<BindGroup> CreateBindGroup(const BindGroupLayout& layout) = 0;
        virtual std::expected<std::unique_ptr<Pipeline>, std::string> CreatePipeline(const PipelineDesc& desc) = 0;

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
