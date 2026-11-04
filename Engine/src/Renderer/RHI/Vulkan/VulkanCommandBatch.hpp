#pragma once

#include "Flux/Renderer/RHI/CommandList.hpp"
#include "Renderer/RHI/Vulkan/Vulkan.hpp"
#include "Renderer/RHI/Vulkan/VulkanRhiCommandList.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Flux::Vk {
    class Device;

    // Command buffers d'une soumission (une image en vol, ou Device::Submit), dans l'ordre où le GPU les joue : la
    // liste principale, coupée à chaque Fork, et entre ses morceaux les listes parallèles. Un pool par command buffer,
    // parce qu'un pool ne sert qu'à un fil à la fois ; pools créés au premier besoin puis gardés (rien par image).
    class CommandBatch {
        public:
        explicit CommandBatch(const Device& device) : m_device(device) {}
        ~CommandBatch();

        CommandBatch(const CommandBatch&) = delete;
        CommandBatch& operator=(const CommandBatch&) = delete;

        // Le GPU a fini la soumission précédente de ce lot : pools vidés, premier command buffer ouvert, à donner à la
        // liste principale (RhiCommandList::Begin). VK_NULL_HANDLE sur une erreur.
        VkCommandBuffer Begin();
        // RhiCommandList::Fork de la liste principale `main` : sa partie enregistrée est fermée, `count` listes
        // parallèles ouvertes à sa suite, et `main` continue dans un nouveau command buffer après elles.
        std::span<RHI::CommandList* const> Fork(RhiCommandList& main, std::uint32_t count);
        // Ferme le dernier morceau de `main` et les listes parallèles. Ensuite : GetCommandBuffers, à soumettre.
        bool End(RhiCommandList& main);
        std::span<const VkCommandBuffer> GetCommandBuffers() const { return m_order; }
        // Cases d'heures écrites par toutes les listes du lot (bit i : case i).
        std::uint32_t GetWrittenTimestamps(const RhiCommandList& main) const;

        private:
        // Au moins `count` command buffers prêts (pools créés) : un Fork n'échoue pas à moitié.
        bool Reserve(std::uint32_t count);
        // Le command buffer suivant, ouvert et placé à la fin de l'ordre de soumission.
        VkCommandBuffer Open();

        struct Slot
        {
            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandBuffer buffer = VK_NULL_HANDLE;
        };

        const Device& m_device;
        std::vector<Slot> m_slots;
        std::uint32_t m_used = 0;
        std::vector<VkCommandBuffer> m_order;
        std::array<RhiCommandList, RHI::MaxParallelCommandLists> m_lists;
        std::array<RHI::CommandList*, RHI::MaxParallelCommandLists> m_listPointers{};
        std::uint32_t m_listCount = 0;
    };
}
