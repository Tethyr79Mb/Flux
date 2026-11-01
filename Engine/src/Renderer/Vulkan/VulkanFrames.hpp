#pragma once

#include "Renderer/RHI/RHITypes.hpp"
#include "Renderer/Vulkan/Vulkan.hpp"

#include <array>
#include <cstdint>

namespace Flux::Vk {
    class Device;

    // Ressources propres à chaque image en vol : command pool et command buffer, sémaphore d'image acquise, barrière
    // (fence) de fin du GPU. Le CPU prépare une image pendant que le GPU dessine l'autre.
    class Frames {
        public:
        struct Frame
        {
            VkCommandPool commandPool = VK_NULL_HANDLE;
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            VkSemaphore imageAvailable = VK_NULL_HANDLE;
            VkFence inFlight = VK_NULL_HANDLE; // signalée à la création : la première attente ne bloque pas
        };

        explicit Frames(const Device& device) : m_device(device) {}
        ~Frames();

        Frames(const Frames&) = delete;
        Frames& operator=(const Frames&) = delete;

        bool Create();
        const Frame& Get(std::uint32_t index) const { return m_frames[index]; }
        // Attend que le GPU ait fini la dernière image soumise dans ces ressources.
        VkResult Wait(std::uint32_t index) const;
        // Une fois sûr de soumettre (sinon l'image suivante attendrait indéfiniment) : barrière remise à zéro, command
        // buffer vidé et ouvert.
        void Begin(std::uint32_t index) const;

        private:
        const Device& m_device;
        std::array<Frame, RHI::FramesInFlight> m_frames{};
    };
}
