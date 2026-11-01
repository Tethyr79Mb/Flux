#include "Renderer/RHI/Vulkan/VulkanCrashMarkers.hpp"

#include "Flux/Core/Version.hpp"
#include "Flux/Platform/Filesystem.hpp"
#include "Renderer/RHI/Vulkan/VulkanDevice.hpp"
#include "Renderer/RHI/Vulkan/VulkanRhiCommandList.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

namespace Flux::Vk {
    CrashMarkers::CrashMarkers(const Device& device, bool windowed) : m_device(device) {
        // Facultatifs : l'image se dessine sans.
        if (windowed && device.HasBufferMarkers()) {
            auto buffer = Buffer::Create(device.GetAllocator(), RHI::FramesInFlight * RHI::MaxSteps * 2 * sizeof(std::uint32_t),
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Readback);
            if (buffer) {
                m_buffer = std::move(*buffer);
            }
        }
    }

    void CrashMarkers::BeginFrame(std::uint32_t frame, RhiCommandList& commands) {
        FrameSteps& steps = m_frames[frame];
        steps.submitted = false;
        if (m_buffer.GetHandle() == VK_NULL_HANDLE) {
            return;
        }
        // Numéro jamais nul : le buffer commence à zéro, une étape jamais atteinte ne lui ressemble pas.
        m_serial = m_serial == UINT32_MAX ? 1 : m_serial + 1;
        steps.serial = m_serial;
        commands.SetStepMarkers({m_buffer.GetHandle(), frame * RHI::MaxSteps * 2 * sizeof(std::uint32_t), steps.serial,
                                 &steps.names, &steps.count});
    }

    bool CrashMarkers::Check(VkResult result, const char* call) {
        if (result == VK_ERROR_DEVICE_LOST) {
            Report();
        }
        return Vk::Check(result, call);
    }

    void CrashMarkers::Report() {
        if (m_reported) {
            return;
        }
        m_reported = true;
        std::string report = std::format("Le GPU s'est bloqué ou a été relancé par le pilote ({}), Flux {}.", m_device.GetName(),
                                         GetFullVersion());
        if (m_buffer.GetHandle() == VK_NULL_HANDLE) {
            report += "\nÉtapes inconnues : ce GPU ne note pas où il en est (pas de VK_AMD_buffer_marker).";
        } else {
            // Images envoyées, de la plus ancienne à la plus récente : la première étape commencée mais pas finie est
            // celle où le GPU s'est arrêté.
            std::array<const FrameSteps*, RHI::FramesInFlight> frames{};
            for (std::uint32_t i = 0; i < RHI::FramesInFlight; ++i) {
                frames[i] = &m_frames[i];
            }
            std::ranges::sort(frames, {}, [](const FrameSteps* frame) { return frame->serial; });
            std::array<std::uint32_t, RHI::FramesInFlight * RHI::MaxSteps * 2> marks{};
            m_buffer.Read(marks.data(), sizeof(marks));
            for (const FrameSteps* frame : frames) {
                if (!frame->submitted || frame->count == 0) {
                    continue;
                }
                const std::size_t first = static_cast<std::size_t>(frame - m_frames.data()) * RHI::MaxSteps * 2;
                report += std::format("\nImage {} :", frame->serial);
                for (std::uint32_t step = 0; step < frame->count; ++step) {
                    const bool started = marks[first + step * 2] == frame->serial;
                    const bool finished = marks[first + step * 2 + 1] == frame->serial;
                    report += std::format("\n  {:<28} {}", frame->names[step],
                                          finished ? "finie" : started ? "COMMENCÉE, PAS FINIE" : "pas commencée");
                }
            }
        }
        if (const std::string fault = m_device.DescribeFault(); !fault.empty()) {
            report += "\nPanne décrite par le pilote : " + fault;
        }
        Log::Error("{}", report);
        // Le programme s'arrête juste après : le rapport reste dans un fichier à côté de l'exe.
        const std::filesystem::path file = Platform::GetExecutableDirectory() / "plantage-gpu.txt";
        if (std::ofstream out(file, std::ios::binary); out << report << '\n') {
            Log::Error("Rapport écrit dans {}", file.string());
        }
    }
}
