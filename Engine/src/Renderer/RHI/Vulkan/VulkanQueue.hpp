#pragma once

#include "Renderer/RHI/Vulkan/Vulkan.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace Flux::Vk {
    // Une file de soumission et son sémaphore timeline : chaque soumission signale la valeur suivante, que le CPU ou
    // une autre file attend pour savoir que tout ce qui la précède est fini (images en vol, libérations différées,
    // copies). File graphique et file de transfert aujourd'hui ; une file de calcul asynchrone prendra la même forme.
    // Deux Queue peuvent partager un VkQueue (copies sur la file graphique quand il n'y a pas de file de transfert) :
    // chacune garde son compteur. Fil principal seulement (une soumission demande l'accès exclusif au VkQueue).
    class Queue {
        public:
        static std::expected<Queue, std::string> Create(VkDevice device, VkQueue queue, std::uint32_t family);

        Queue() = default; // vide : rien à détruire
        ~Queue();
        Queue(Queue&& other) noexcept;
        Queue& operator=(Queue&& other) noexcept;
        Queue(const Queue&) = delete;
        Queue& operator=(const Queue&) = delete;

        VkQueue GetHandle() const { return m_queue; }
        std::uint32_t GetFamily() const { return m_family; }

        // Soumet `commands` après `waits`, signale `signals` puis la valeur suivante du timeline (GetSubmitted après
        // succès). Sur échec, rien n'est soumis et le compteur ne bouge pas. signals : au plus MaxSignals.
        static constexpr std::size_t MaxSignals = 3;
        VkResult Submit(std::span<const VkSemaphoreSubmitInfo> waits, std::span<const VkCommandBufferSubmitInfo> commands,
                        std::span<const VkSemaphoreSubmitInfo> signals = {});
        // Attente à joindre à la soumission d'une autre file : tout ce que celle-ci a soumis jusqu'à `value`.
        VkSemaphoreSubmitInfo GetWait(std::uint64_t value) const;

        // Dernière valeur soumise (0 avant la première soumission).
        std::uint64_t GetSubmitted() const { return m_submitted; }
        // Dernière valeur atteinte par le GPU (lue au pilote à chaque appel).
        std::uint64_t GetCompleted() const;
        bool IsDone(std::uint64_t value) const { return value == 0 || GetCompleted() >= value; }
        // Bloque le CPU jusqu'à `value` (0 : rend tout de suite).
        VkResult Wait(std::uint64_t value) const;

        private:
        void Reset();

        VkDevice m_device = VK_NULL_HANDLE;
        VkQueue m_queue = VK_NULL_HANDLE;
        std::uint32_t m_family = 0;
        VkSemaphore m_timeline = VK_NULL_HANDLE;
        std::uint64_t m_submitted = 0;
    };
}
