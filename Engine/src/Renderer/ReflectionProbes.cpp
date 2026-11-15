#include "Renderer/ReflectionProbes.hpp"

#include "Flux/Core/Log.hpp"

#include <algorithm>
#include <bit>

namespace Flux {
    namespace {
        // La sonde a bougé de plus (m) : refaite. En dessous, une entité recalculée au bit près ne la refait pas.
        constexpr double MovedDistance = 0.01;
        // Capture attendue au plus (s) pendant un chargement : une scène qui charge sans fin (planète, flux de modèles)
        // a quand même ses reflets, refaits à la fin du chargement.
        constexpr double LoadingPatience = 3.0;
    }

    std::expected<std::unique_ptr<ReflectionProbes>, std::string> ReflectionProbes::Create(RHI::Device& device,
                                                                                            EnvironmentRenderer& environment) {
        std::unique_ptr<ReflectionProbes> probes(new ReflectionProbes(device, environment));
        auto cube = device.CreateCubeRenderTarget(Format, Size, static_cast<std::uint32_t>(std::bit_width(Size)));
        auto depth = device.CreateRenderTarget(RHI::Format::D32Float, {Size, Size});
        if (!cube || !depth) {
            return std::unexpected(!cube ? cube.error() : depth.error());
        }
        probes->m_captureCube = std::move(*cube);
        probes->m_captureDepth = std::move(*depth);
        probes->m_captureGroup = environment.CreateSourceGroup(*probes->m_captureCube);
        if (!probes->m_captureGroup) {
            return std::unexpected("allocation du groupe de la capture des sondes");
        }
        return probes;
    }

    ReflectionProbes::~ReflectionProbes() = default;

    bool ReflectionProbes::NeedsCapture(const Slot& slot, const Probe& probe, const Math::WorldPosition& origin,
                                        const Lighting& lighting, bool modelsLoading, double seconds) const {
        const float refresh = probe.component->refresh;
        if (refresh > 0.0f && seconds - slot.capturedAt >= refresh) {
            return true;
        }
        // En plein chargement, une sonde déjà prise garde ce qu'elle a ; une nouvelle attend un peu les objets.
        if (modelsLoading && (slot.ready || seconds - m_loadingSince < LoadingPatience)) {
            return false;
        }
        return !slot.ready || Math::Length(Math::Delta(slot.origin, origin)) > MovedDistance || slot.lighting != lighting
            || slot.loads != m_loads;
    }

    void ReflectionProbes::Prepare(std::span<const Probe> probes, const Math::WorldPosition& camera, const Lighting& lighting,
                                   bool modelsLoading, double seconds) {
        m_capture.reset();
        m_readyCount = 0;
        m_seconds = seconds;
        m_lighting = lighting;
        if (modelsLoading && !m_wasLoading) {
            m_loadingSince = seconds;
        }
        if (!modelsLoading && m_wasLoading) {
            ++m_loads;
        }
        m_wasLoading = modelsLoading;

        // Chaque sonde dans sa case de l'image précédente, sinon dans une case libre à cette image (à capturer).
        for (Slot& slot : m_slots) {
            slot.used = false;
        }
        std::array<std::uint32_t, MaxProbes> slotOf{};
        const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(probes.size(), MaxProbes));
        std::array<bool, MaxProbes> placed{};
        for (std::uint32_t p = 0; p < count; ++p) {
            for (std::uint32_t s = 0; s < MaxProbes; ++s) {
                if (!m_slots[s].used && m_slots[s].entity == probes[p].transform.entity) {
                    m_slots[s].used = true;
                    slotOf[p] = s;
                    placed[p] = true;
                    break;
                }
            }
        }
        for (std::uint32_t p = 0; p < count; ++p) {
            if (placed[p]) {
                continue;
            }
            for (std::uint32_t s = 0; s < MaxProbes; ++s) {
                if (!m_slots[s].used && m_slots[s].entity != probes[p].transform.entity) {
                    Slot& slot = m_slots[s];
                    slot.used = true;
                    slot.entity = probes[p].transform.entity;
                    slot.ready = false;
                    slotOf[p] = s;
                    placed[p] = true;
                    break;
                }
            }
        }

        for (std::uint32_t p = 0; p < count; ++p) {
            if (!placed[p]) {
                continue;
            }
            Slot& slot = m_slots[slotOf[p]];
            if (!slot.reflections) {
                auto reflections = m_device.CreateCubeRenderTarget(Format, Size, m_environment.GetReflectionMips());
                if (!reflections) {
                    Log::Error("Sonde de reflets : {}", reflections.error());
                    continue;
                }
                slot.reflections = std::move(*reflections);
            }
            Math::WorldPosition origin = camera;
            origin += probes[p].transform.position;
            if (!m_capture && NeedsCapture(slot, probes[p], origin, lighting, modelsLoading, seconds)) {
                m_capture = Math::Vector3f(probes[p].transform.position);
                m_captureSlot = slotOf[p];
                m_captureOrigin = origin;
            }
            if (slot.ready) {
                m_ready[m_readyCount++] = {slotOf[p], &probes[p], Math::Vector3f(Math::Delta(camera, slot.origin))};
            }
        }
    }

    void ReflectionProbes::FinishCapture(RHI::CommandList& commands, std::uint32_t frameIndex) {
        if (!m_capture) {
            return;
        }
        commands.GenerateMipmaps(*m_captureCube); // le laisse lisible
        Slot& slot = m_slots[m_captureSlot];
        m_environment.FilterProbe(commands, frameIndex, *m_captureGroup, Size, *slot.reflections);
        slot.ready = true;
        slot.origin = m_captureOrigin;
        slot.lighting = m_lighting;
        slot.loads = m_loads;
        slot.capturedAt = m_seconds;
        m_capture.reset();
    }
}
