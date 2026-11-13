#pragma once

#include "Flux/Renderer/RHI/RHITypes.hpp"
#include "Renderer/RHI/Vulkan/Vulkan.hpp"

#include <array>
#include <cstdint>

namespace Flux::Vk {
    class Device;

    // Ressources propres à chaque image en vol : sémaphore d'image acquise et valeur du timeline graphique portée par
    // sa dernière soumission (Transfer::GetSignal). Le CPU prépare une image pendant que le GPU dessine l'autre ; pas
    // de fence : le timeline signalé par chaque soumission graphique suffit, et ne se remet jamais à zéro. Les command
    // buffers sont dans un CommandBatch par image.
    class Frames {
        public:
        struct Frame
        {
            VkSemaphore imageAvailable = VK_NULL_HANDLE; // binaire : vkAcquireNextImageKHR n'accepte pas de timeline
            std::uint64_t graphicsValue = 0;             // 0 : rien de soumis, l'attente ne bloque pas
        };

        explicit Frames(const Device& device) : m_device(device) {}
        ~Frames();

        Frames(const Frames&) = delete;
        Frames& operator=(const Frames&) = delete;

        bool Create();
        const Frame& Get(std::uint32_t index) const { return m_frames[index]; }
        // Attend que le GPU ait fini la dernière image soumise dans ces ressources.
        VkResult Wait(std::uint32_t index) const;
        // Après une soumission réussie : la valeur qu'elle a signalée. Sur échec, rien : l'ancienne valeur reste
        // atteinte, l'image suivante dans ces ressources n'attendra pas indéfiniment.
        void Submitted(std::uint32_t index, std::uint64_t graphicsValue) { m_frames[index].graphicsValue = graphicsValue; }

        private:
        const Device& m_device;
        std::array<Frame, RHI::FramesInFlight> m_frames{};
    };
}
