#pragma once

#include "Flux/Math/WorldPosition.hpp"
#include "Flux/Renderer/RHI/RHI.hpp"
#include "Flux/Scene/Components.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Renderer/EnvironmentRenderer.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace Flux {
    // Sondes de reflets fixes (ReflectionProbeComponent) : chacune garde sa texture cube de reflets, préfiltrée comme
    // celle du ciel (même nombre de niveaux : Mesh.slang lit les deux de la même façon). Une capture par image au plus,
    // dessinée par MeshRenderer::CaptureProbe dans une texture cube de travail commune, puis préfiltrée par
    // EnvironmentRenderer::FilterProbe dans celle de la sonde. Ici : qui capturer et quand, et les textures.
    // Mémoire : 256² x 6 faces x 8 octets plus les mips, environ 4 Mo par sonde, plus 6 Mo de travail.
    // Coût : rien entre deux captures ; une capture dessine six fois les objets autour (quelques millisecondes d'un
    // coup sur une scène chargée, à mesurer dans le panneau Moteur). Configuration minimum : la même chose.
    class ReflectionProbes {
        public:
        // Doit correspondre à MaxProbes et aux liaisons probeMap0 à 3 dans shaders/Mesh.slang.
        static constexpr std::uint32_t MaxProbes = 4;
        static constexpr std::uint32_t Size = 256;
        static constexpr RHI::Format Format = EnvironmentRenderer::Format;
        // Objets dessinés dans une capture : ceux dont la sphère touche ce rayon autour de la sonde (m).
        static constexpr float CaptureRange = 200.0f;

        // Une sonde de la scène retenue pour l'image, relative à la caméra.
        struct Probe
        {
            RelativeTransform transform;
            const ReflectionProbeComponent* component;
            float distance; // de la caméra à sa boîte : les plus proches d'abord s'il y en a trop
        };

        // Ce dont la capture dépend hors de la scène : refaite quand cela change.
        struct Lighting
        {
            Math::Vector3f toSun;
            Math::Vector3f sunColor;
            float ambient = 0.0f;
            EnvironmentSource source;
            bool operator==(const Lighting&) const = default;
        };

        // Une sonde prête (déjà capturée) pour la lecture : sa case et ce que la lecture en sait.
        struct Ready
        {
            std::uint32_t slot;
            const Probe* probe;
            Math::Vector3f origin; // point de la capture, relatif à la caméra
        };

        static std::expected<std::unique_ptr<ReflectionProbes>, std::string> Create(RHI::Device& device,
                                                                                     EnvironmentRenderer& environment);
        ~ReflectionProbes();
        ReflectionProbes(const ReflectionProbes&) = delete;
        ReflectionProbes& operator=(const ReflectionProbes&) = delete;

        // Dans MeshRenderer::Prepare : les sondes de l'image (MaxProbes au plus) ; choisit celle à capturer à cette image.
        // modelsLoading : des modèles ou des images sont en cours de lecture (capturer attendrait qu'ils arrivent).
        void Prepare(std::span<const Probe> probes, const Math::WorldPosition& camera, const Lighting& lighting,
                     bool modelsLoading, double seconds);
        // Sondes prêtes, dans l'ordre de `probes`, valables jusqu'au prochain Prepare.
        std::span<const Ready> GetReady() const { return {m_ready.data(), m_readyCount}; }
        // Point de la sonde à capturer à cette image, relatif à la caméra.
        std::optional<Math::Vector3f> GetCapture() const { return m_capture; }

        // Cibles de la capture : la face f du niveau 0, et la profondeur (de la taille d'une face).
        const RHI::Texture& GetCaptureCube() const { return *m_captureCube; }
        const RHI::Texture& GetCaptureDepth() const { return *m_captureDepth; }
        // Après le dessin des six faces (laissées cibles) : réduit la capture, la préfiltre dans les reflets de la sonde
        // (laissés lisibles), qui sera lue dès l'image suivante.
        void FinishCapture(RHI::CommandList& commands, std::uint32_t frameIndex);

        const RHI::Texture& GetReflections(std::uint32_t slot) const { return *m_slots[slot].reflections; }

        private:
        struct Slot
        {
            Entity entity = NullEntity;
            bool used = false;     // retenue à cette image
            bool ready = false;    // capturée au moins une fois pour cette entité
            Math::WorldPosition origin;
            Lighting lighting;
            std::uint64_t loads = 0; // m_loads à la capture
            double capturedAt = 0.0;
            std::unique_ptr<RHI::Texture> reflections;
        };

        ReflectionProbes(RHI::Device& device, EnvironmentRenderer& environment) : m_device(device), m_environment(environment) {}
        bool NeedsCapture(const Slot& slot, const Probe& probe, const Math::WorldPosition& origin, const Lighting& lighting,
                          bool modelsLoading, double seconds) const;

        RHI::Device& m_device;
        EnvironmentRenderer& m_environment;
        std::array<Slot, MaxProbes> m_slots;
        std::array<Ready, MaxProbes> m_ready{};
        std::uint32_t m_readyCount = 0;
        std::unique_ptr<RHI::Texture> m_captureCube;  // avec tous ses mips : le préfiltrage lit celui qui couvre son angle
        std::unique_ptr<RHI::Texture> m_captureDepth;
        std::unique_ptr<RHI::BindGroup> m_captureGroup; // m_captureCube, lu par le préfiltrage (EnvironmentRenderer)
        std::optional<Math::Vector3f> m_capture;
        std::uint32_t m_captureSlot = 0;
        Math::WorldPosition m_captureOrigin;
        Lighting m_lighting;
        double m_seconds = 0.0;
        // Fins de chargement vues (des modèles en lecture, puis plus aucun) : chacune refait les sondes, qui n'avaient
        // pas encore les objets arrivés.
        std::uint64_t m_loads = 0;
        bool m_wasLoading = false;
        double m_loadingSince = 0.0;
    };
}
