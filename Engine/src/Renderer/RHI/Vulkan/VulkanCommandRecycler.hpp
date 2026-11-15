#pragma once

#include "Renderer/RHI/Vulkan/Vulkan.hpp"

#include <cstdint>
#include <deque>
#include <expected>
#include <string>
#include <utility>
#include <vector>

namespace Flux::Vk {
    class Queue;

    // Command buffers primaires d'une file, réutilisés : rendus avec la valeur du timeline qui les finit, recommencés
    // une fois la file arrivée là. Pas d'allocation ni de libération par envoi, et une seule règle de fin de vie au
    // lieu d'un vkFreeCommandBuffers à chaque chemin. La Queue doit vivre plus longtemps. Fil principal seulement.
    class CommandRecycler {
        public:
        static std::expected<CommandRecycler, std::string> Create(VkDevice device, const Queue& queue);

        CommandRecycler() = default; // vide : rien à détruire
        ~CommandRecycler();
        CommandRecycler(CommandRecycler&& other) noexcept;
        CommandRecycler& operator=(CommandRecycler&& other) noexcept;
        CommandRecycler(const CommandRecycler&) = delete;
        CommandRecycler& operator=(const CommandRecycler&) = delete;

        // Command buffer commencé (ONE_TIME_SUBMIT) ; VK_NULL_HANDLE sur échec.
        VkCommandBuffer Begin();
        // Soumis : réutilisable quand la file atteint `value` (valeurs croissantes d'un appel à l'autre).
        void Retire(VkCommandBuffer commands, std::uint64_t value);
        // Jamais soumis, ou soumission attendue : réutilisable tout de suite.
        void Release(VkCommandBuffer commands);

        private:
        void Reset();

        VkDevice m_device = VK_NULL_HANDLE;
        const Queue* m_queue = nullptr;
        VkCommandPool m_pool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> m_free;
        std::deque<std::pair<VkCommandBuffer, std::uint64_t>> m_pending; // valeur de la file qui le libère
    };
}
