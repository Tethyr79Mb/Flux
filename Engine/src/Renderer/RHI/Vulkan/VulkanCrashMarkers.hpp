#pragma once

#include "Flux/Renderer/RHI/RHITypes.hpp"
#include "Renderer/RHI/Vulkan/Vulkan.hpp"
#include "Renderer/RHI/Vulkan/VulkanBuffer.hpp"

#include <array>
#include <cstdint>

namespace Flux::Vk {
    class Device;
    class RhiCommandList;

    // Diagnostic de plantage du GPU : les étapes de chaque image (CommandList::BeginStep) notées par le GPU au début
    // et à la fin (VK_AMD_buffer_marker), et à la perte du device un rapport (journal et plantage-gpu.txt) qui dit où
    // il s'est arrêté, avec la description de la panne par le pilote quand il la donne.
    class CrashMarkers {
        public:
        // windowed faux : pas d'images, pas de repères (le rapport reste possible).
        CrashMarkers(const Device& device, bool windowed);

        // Image qui commence dans les ressources `frame` : son numéro, et les repères donnés à sa liste.
        void BeginFrame(std::uint32_t frame, RhiCommandList& commands);
        void Submitted(std::uint32_t frame) { m_frames[frame].submitted = true; }
        // Appel Vulkan d'une image : sur la perte du device (GPU bloqué, pilote relancé), écrit le rapport une fois.
        bool Check(VkResult result, const char* call);

        private:
        void Report();

        struct FrameSteps
        {
            // Étapes de la dernière image soumise dans ces ressources (BeginStep).
            std::array<const char*, RHI::MaxSteps> names{};
            std::uint32_t count = 0;
            std::uint32_t serial = 0; // numéro de cette image, écrit par le GPU au début et à la fin de chaque étape
            bool submitted = false;
        };

        const Device& m_device;
        Buffer m_buffer; // FramesInFlight x MaxSteps x (début, fin) ; vide sans VK_AMD_buffer_marker
        std::array<FrameSteps, RHI::FramesInFlight> m_frames{};
        std::uint32_t m_serial = 0;
        bool m_reported = false;
    };
}
