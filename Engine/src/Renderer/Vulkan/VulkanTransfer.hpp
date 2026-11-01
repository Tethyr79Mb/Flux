#pragma once

#include "Renderer/Vulkan/Vulkan.hpp"
#include "Renderer/Vulkan/VulkanBuffer.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Flux::Vk {
    // Envois vers la mémoire vidéo. Les copies partent sur la file de transfert dédiée quand la carte en a une (moteur
    // de copie : elles tournent pendant le dessin au lieu de le retarder) ; sinon sur la file graphique, même chemin.
    // Les données passent par un anneau d'envoi mappé en permanence (pas d'allocation par envoi) ; un envoi plus gros
    // que la moitié de l'anneau a son propre buffer. Deux sémaphores timeline ordonnent les files : « transfert »
    // (copies finies) et « graphique » (images et calculs finis).
    // Fil principal seulement.
    class Transfer {
        public:
        struct Setup
        {
            VkDevice device = VK_NULL_HANDLE;
            VmaAllocator allocator = VK_NULL_HANDLE;
            VkQueue graphicsQueue = VK_NULL_HANDLE;
            std::uint32_t graphicsFamily = 0;
            // VK_NULL_HANDLE : pas de file de transfert séparée, les copies passent par la file graphique.
            VkQueue transferQueue = VK_NULL_HANDLE;
            std::uint32_t transferFamily = 0;
        };

        static std::expected<std::unique_ptr<Transfer>, std::string> Create(const Setup& setup);
        ~Transfer();

        Transfer(const Transfer&) = delete;
        Transfer& operator=(const Transfer&) = delete;

        // Copies sur une file à part (sinon sur la file graphique).
        bool HasDedicatedQueue() const { return m_separate; }
        // Familles qui partagent les buffers remplis par envoi (VK_SHARING_MODE_CONCURRENT) ; vide avec une seule
        // file. Les images, elles, changent de file par un transfert de propriété : partagées, elles perdraient leur
        // compression (DCC des Radeon).
        std::span<const std::uint32_t> GetSharedFamilies() const;

        // Envois asynchrones : ceux faits entre BeginAsync et EndAsync ne retiennent aucune image. Leur partie
        // graphique (prise des images, mipmaps) part avec la première soumission qui suit la fin de leurs copies,
        // relevée sans attendre ; avant, les ressources envoyées ne doivent pas servir. EndAsync rend un numéro pour
        // IsReady (0 : rien envoyé, prêt). Les autres envois sont attendus par la prochaine soumission.
        void BeginAsync();
        std::uint64_t EndAsync();
        bool IsReady(std::uint64_t ticket) const { return ticket <= m_asyncAcquired; }

        // Copie `data` dans `target` à `offset`. inUse : le buffer sert peut-être déjà à une image soumise, la copie
        // attend la fin de ce qui a été soumis à la file graphique (sinon elle réécrirait ce qu'une image lit).
        // Un buffer neuf, ou une plage qu'aucune image soumise ne lit (inUse faux), ne retient rien.
        bool WriteBuffer(VkBuffer target, VkDeviceSize offset, const void* data, VkDeviceSize size, bool inUse);

        // Image neuve (jamais utilisée) remplie par `regions`, dont bufferOffset compte depuis le début de `pixels`
        // (ou de sourceOffset dans `source`, mémoire d'envoi remplie par l'appelant : rien n'est recopié, elle doit
        // vivre jusqu'à la fin de la copie, voir KeepUntilCopied). generateMips : les niveaux 1..levels-1 sont réduits
        // du niveau 0 par la file graphique (le blit est interdit sur une file de transfert). L'image finit en
        // SHADER_READ_ONLY_OPTIMAL sur la file graphique.
        struct ImageWrite
        {
            VkImage image = VK_NULL_HANDLE;
            VkExtent2D extent{};
            std::uint32_t levels = 1;
            std::uint32_t layers = 1;
            bool generateMips = false;
            std::span<const VkBufferImageCopy> regions;
        };
        bool WriteImage(const ImageWrite& write, std::span<const std::uint8_t> pixels);
        bool WriteImage(const ImageWrite& write, VkBuffer source, VkDeviceSize sourceOffset);
        // Garde `buffer` (source de copies déjà enregistrées) jusqu'à la fin des copies en cours, puis le détruit.
        void KeepUntilCopied(Buffer buffer);

        // Ce que la prochaine soumission à la file graphique doit emporter : soumet d'abord les copies en cours.
        // commands (terminés, à mettre devant les commandes de la soumission) prennent les images à la file de
        // transfert et calculent leurs mipmaps ; transferValue est l'attente des copies. Rendre par
        // GraphicsSubmitted, même sur échec ; valable jusque-là.
        struct GraphicsPart
        {
            std::span<const VkCommandBuffer> commands;
            std::uint64_t transferValue = 0; // 0 : rien à attendre
            std::uint64_t asyncSerial = 0;   // dernier lot asynchrone emporté (0 : aucun)
        };
        GraphicsPart TakeGraphics();
        // Attente à joindre à la soumission (sémaphore transfert, toutes étapes) ; part.transferValue non nul.
        VkSemaphoreSubmitInfo GetWait(const GraphicsPart& part) const;
        // Signal à joindre à chaque soumission à la file graphique : la valeur qu'elle porte (GetSignal().value).
        VkSemaphoreSubmitInfo GetSignal() const;
        // Après vkQueueSubmit2 : submitted faux, la soumission n'a pas eu lieu (le signal n'arrivera jamais).
        void GraphicsSubmitted(GraphicsPart& part, bool submitted);
        // Attend que la file graphique ait fini la dernière soumission (Device::ImmediateSubmit).
        bool WaitGraphics() const;

        // Libération différée (RHI::Device::Retire) : une ressource lâchée maintenant peut servir à ce qui a été
        // soumis ou enregistré. Elle est libre quand la file graphique a atteint `graphics` et la file de transfert
        // `transfer` (IsDone).
        struct Marks
        {
            std::uint64_t graphics = 0;
            std::uint64_t transfer = 0;
        };
        Marks GetMarks() const;
        bool IsDone(const Marks& marks) const;

        // Rend ce que le GPU a fini : place dans l'anneau, buffers d'envoi, command buffers. Une fois par image.
        void Collect();

        private:
        Transfer() = default;

        struct Batch // copies en cours d'enregistrement, puis en vol sur la file de transfert
        {
            VkCommandBuffer commands = VK_NULL_HANDLE;
            std::vector<Buffer> staging; // envois trop gros pour l'anneau, mémoire d'envoi de l'appelant
            VkDeviceSize bytes = 0;
            // Première place prise dans l'anneau (en octets depuis le début, comme m_ringHead) : l'anneau n'est rendu
            // que jusqu'à la plus ancienne place d'un lot pas fini (les deux flots se mêlent dans l'anneau).
            std::uint64_t ringStart = UINT64_MAX;
            std::uint64_t graphicsWait = 0;  // valeur graphique à attendre avant de copier (0 : aucune)
            std::uint64_t value = 0;         // valeur transfert signalée
        };
        // Deux flots : les envois attendus par la prochaine soumission, et les asynchrones (BeginAsync), soumis à
        // part pour que la prochaine image n'attende pas leurs copies.
        struct Stream
        {
            Batch batch;
            VkCommandBuffer graphics = VK_NULL_HANDLE; // partie graphique en cours d'enregistrement
        };
        // Partie graphique d'un lot asynchrone soumis : part quand ses copies sont finies.
        struct AsyncPart
        {
            VkCommandBuffer commands = VK_NULL_HANDLE; // nul : rien à prendre (buffers seulement)
            std::uint64_t transferValue = 0;
            std::uint64_t serial = 0;
        };

        struct Staged
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
        };
        Stream& Current() { return m_streams[m_async ? 1 : 0]; }
        // Place pour `size` octets, déjà remplie de `data` : dans l'anneau, sinon dans un buffer à part du lot.
        std::expected<Staged, std::string> Stage(const void* data, VkDeviceSize size, VkDeviceSize alignment);
        // Commence le lot de copies (et la partie graphique) au premier envoi.
        bool BeginBatch();
        bool BeginGraphics();
        // Copie d'une image et sa partie graphique ; source déjà remplie, regions comptées depuis son début.
        bool RecordImage(const ImageWrite& write, VkBuffer source, VkDeviceSize sourceOffset, VkDeviceSize bytes);
        // Soumet le lot de copies d'un flot à la file de transfert.
        bool Flush(Stream& stream);
        void ReleaseBatch(Batch& batch);
        void UpdateRingTail();

        VkDevice m_device = VK_NULL_HANDLE;
        VmaAllocator m_allocator = VK_NULL_HANDLE;
        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        VkQueue m_transferQueue = VK_NULL_HANDLE; // la file graphique sans file séparée
        std::array<std::uint32_t, 2> m_families{}; // graphique, transfert
        bool m_separate = false;

        VkCommandPool m_transferPool = VK_NULL_HANDLE;
        VkCommandPool m_graphicsPool = VK_NULL_HANDLE;
        VkSemaphore m_transferTimeline = VK_NULL_HANDLE;
        VkSemaphore m_graphicsTimeline = VK_NULL_HANDLE;
        std::uint64_t m_transferValue = 0; // dernière valeur soumise
        std::uint64_t m_graphicsValue = 0;

        Buffer m_ring;
        std::uint64_t m_ringHead = 0; // octets réservés depuis le début (position : modulo la taille)
        std::uint64_t m_ringTail = 0; // octets rendus

        std::array<Stream, 2> m_streams; // attendus, asynchrones
        bool m_async = false;
        bool m_asyncWritten = false;       // un envoi depuis BeginAsync
        std::uint64_t m_asyncSerial = 0;   // dernier lot asynchrone soumis
        std::uint64_t m_asyncAcquired = 0; // dernier lot asynchrone pris par une soumission graphique
        std::deque<Batch> m_inFlight;
        std::deque<std::pair<std::uint64_t, Buffer>> m_kept; // KeepUntilCopied : valeur transfert qui les libère
        std::deque<AsyncPart> m_asyncPending;
        std::uint64_t m_graphicsNeeds = 0; // valeur transfert que la prochaine soumission attend
        // Partie rendue par TakeGraphics (réutilisés : pas d'allocation par image).
        std::vector<VkCommandBuffer> m_takenCommands;
        std::vector<AsyncPart> m_takenAsync;
        VkCommandBuffer m_takenSync = VK_NULL_HANDLE;
        struct GraphicsCommands
        {
            VkCommandBuffer commands = VK_NULL_HANDLE;
            std::uint64_t value = 0; // valeur graphique qui la finit
        };
        std::deque<GraphicsCommands> m_graphicsInFlight;
    };
}
