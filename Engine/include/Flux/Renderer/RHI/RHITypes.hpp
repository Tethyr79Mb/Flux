#pragma once

// Types communs du RHI : énumérations et petits types valeur, en C++ pur (aucun en-tête d'une API graphique).

#include <cstdint>
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
        R8Unorm,     // une valeur de 0 à 1 (masque)
        // Compressés par blocs de 4 x 4 pixels, 16 octets chacun (1 octet par pixel au lieu de 4) : textures lues
        // souvent (sol des planètes). Device::HasTextureCompressionBC.
        BC7Unorm,
        BC7Srgb,
    };

    enum class BufferUsage : std::uint8_t {
        Vertex = 1 << 0,
        Index = 1 << 1,   // indices 32 bits
        Storage = 1 << 2, // lu par les shaders (matrices des os), écrit par les compute shaders
        Indirect = 1 << 3, // commandes de dessin lues par le GPU (CommandList::DrawIndexedIndirect)
        // Sommets et indices lus par la construction des structures du ray tracing (AccelerationGeometry), et
        // mémoire de travail de cette construction. Seulement si Device::HasRayTracing.
        AccelerationInput = 1 << 4,
        AccelerationScratch = 1 << 5,
    };

    enum class ShaderStage : std::uint8_t {
        // Étape de la géométrie : les sommets, ou les étapes de tâche et de maillage qui la remplacent dans un
        // pipeline à mesh shaders (Device::HasMeshShaders) ; une ressource vue des sommets l'est aussi d'elles.
        Vertex = 1 << 0,
        Fragment = 1 << 1,
        Compute = 1 << 2,
    };

    // Qui lit, après CommandList::ComputeBarrier, ce que les Dispatch précédents ont écrit. Les calculs suivants
    // toujours (Compute seul) ; ne demander que ce qui sert : Host oblige le GPU à réécrire son cache L2 en mémoire,
    // Draw à vider les caches des commandes indirectes et des sommets.
    enum class ComputeReaders : std::uint8_t {
        Compute = 0,
        Draw = 1 << 0, // commandes indirectes, sommets, index, shaders des dessins (tâche et maillage compris)
        Host = 1 << 1, // le CPU (MemoryType::Readback), après Device::Submit ou deux images plus tard
    };

    // Opérateurs de bits pour les énumérations de drapeaux.
    template <typename E>
    inline constexpr bool IsFlagEnum = false;
    template <>
    inline constexpr bool IsFlagEnum<BufferUsage> = true;
    template <>
    inline constexpr bool IsFlagEnum<ShaderStage> = true;
    template <>
    inline constexpr bool IsFlagEnum<ComputeReaders> = true;

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

    enum class Filter : std::uint8_t { Nearest, Linear };
    enum class AddressMode : std::uint8_t { Repeat, ClampToEdge };

    // Une ressource d'un groupe : texture échantillonnée (avec son sampler), buffer de stockage (lu par les shaders de
    // dessin, lu et écrit par les compute shaders), ou table de textures (`count` cases, `Sampler2D t[]` dans les
    // shaders) : toutes les images des matériaux dans un seul groupe, choisies par indice dans le shader. Les cases non
    // remplies ne doivent pas être lues ; une case que le GPU ne lit pas peut être remplie pendant qu'il dessine.
    // StorageTexture : texture écrite (et lue) pixel par pixel par les compute shaders, son niveau 0 seulement
    // (`RWTexture3D` dans les shaders ; CreateVolumeTexture), sans sampler.
    // AccelerationStructure : structure haute du ray tracing (Device::HasRayTracing), lue par les ray queries.
    enum class BindingType : std::uint8_t { Texture, StorageBuffer, TextureTable, StorageTexture, AccelerationStructure };

    enum class CullMode : std::uint8_t { None, Back, Front }; // faces avant : sens antihoraire
    enum class CompareOp : std::uint8_t { Greater, GreaterOrEqual, Less, LessOrEqual, Always };
    enum class BlendMode : std::uint8_t {
        Opaque,
        Alpha, // mélange classique, couleur non prémultipliée (interface, texte)
        Additive, // couleur ajoutée à ce qui est derrière (lumière : flammes, lueurs), dans n'importe quel ordre
        // Couleur déjà multipliée par son alpha, ajoutée à ce qui est derrière assombri de cet alpha : une surface peut
        // ajouter de la lumière sans rien cacher (reflets d'un verre presque transparent).
        Premultiplied,
    };

    // Ce qu'une passe fait d'une texture : le graphe de rendu le déclare (RenderGraph), CommandList::UseTextures en
    // déduit les barrières. Une texture remplie par envoi (CreateTexture…) commence en Sampled, une cible en None.
    enum class TextureAccess : std::uint8_t {
        None,        // jamais utilisée : contenu indéfini
        ColorTarget, // cible de couleur d'une passe de dessin
        DepthTarget, // tampon de profondeur d'une passe de dessin
        Sampled,     // lue par les shaders (sommets, pixels, calcul)
        Storage,     // écrite par les compute shaders (BindingType::StorageTexture)
    };

    inline constexpr std::uint32_t MaxTimestamps = 48;
    inline constexpr std::uint32_t MaxSteps = 48;

    enum class PresentMode : std::uint8_t { Fifo, FifoRelaxed, Mailbox, Immediate, Other };

    enum class FrameStatus : std::uint8_t {
        Ready, // la liste de commandes attend
        Skip,  // rien à dessiner cette fois (swapchain recréée) : réessayer à l'image suivante
        Error, // erreur irrécupérable (device perdu, etc.)
    };
}
