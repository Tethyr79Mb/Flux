#pragma once

#include "Flux/Renderer/AmbientOcclusion.hpp"
#include "Flux/Renderer/RHI/RHI.hpp"
#include "Renderer/MeshRenderer.hpp"

#include <expected>
#include <memory>
#include <string>

namespace Flux {
    // Occlusion ambiante GTAO (shaders/AmbientOcclusion.slang) des pixels du G-buffer, entre sa passe et celle de
    // l'éclairage, qui en assombrit la seule lumière ambiante (ciel, sondes, intérieurs, air, rebond du sol) : Compute
    // cherche l'horizon autour de chaque pixel dans la profondeur, Denoise lisse sur 5 x 5 sans traverser les bords.
    // Le bruit change à chaque image : l'anticrénelage temporel en fait la moyenne.
    // Coût : Haute, 3 tranches de 3 pas des deux côtés (18 lectures de profondeur) par pixel de rendu ; Moyenne,
    // 2 tranches de 3 pas sur un pixel sur quatre. Les pixels lointains (rayon sous 1,5 pixel) sortent aussitôt. Pas de
    // niveaux de mip de la profondeur (le RHI n'écrit pas encore un niveau à la fois) : le rayon à l'écran est borné
    // (MaxScreenRadius) pour que les lectures restent proches dans le cache.
    class AmbientOcclusionRenderer {
        public:
        static std::expected<std::unique_ptr<AmbientOcclusionRenderer>, std::string> Create(RHI::Device& device);

        // Format des deux textures de travail (à la taille de rendu) : occlusion, distance devant la caméra.
        static constexpr RHI::Format WorkFormat = RHI::Format::RG16Float;
        // Réglages de la scène (composant Ciel).
        struct Settings
        {
            float radius = 2.0f;    // m : portée des obstacles (1 m ne touchait pas le dessous d'un vaisseau)
            float intensity = 1.0f; // 0 : aucune occlusion, 1 : celle de XeGTAO
            float falloff = 0.6f;   // part de la fin du rayon où un obstacle compte de moins en moins
        };
        struct Targets
        {
            const RHI::Texture* depth = nullptr;   // profondeur de la scène
            const RHI::Texture* base = nullptr;    // G-buffer : sorte de matière en alpha
            const RHI::Texture* normals = nullptr; // G-buffer : normales
            const RHI::Texture* raw = nullptr;     // WorkFormat : occlusion brute, puis lissée (lue par l'éclairage)
            const RHI::Texture* filtered = nullptr;
        };
        // Après chaque changement de taille des textures (GPU à l'arrêt).
        void Bind(const Targets& targets);

        // Hors d'une passe, chacun dans la sienne (le graphe de rendu met les barrières entre elles).
        void Compute(RHI::CommandList& commands, const MeshRenderer::RayShadowView& view, const Settings& settings) const;
        void Denoise(RHI::CommandList& commands, const MeshRenderer::RayShadowView& view, const Settings& settings) const;

        void SetQuality(AmbientOcclusionQuality quality) { m_quality = quality; }
        AmbientOcclusionQuality GetQuality() const { return m_quality; }
        bool IsActive() const { return m_quality != AmbientOcclusionQuality::Off; }
        // Pas entre deux pixels calculés (1 ou 2), lu par l'éclairage pour agrandir la demi-résolution ; 0 sans occlusion.
        std::uint32_t GetStride() const;

        private:
        explicit AmbientOcclusionRenderer(RHI::Device& device) : m_device(device) {}
        void Run(RHI::CommandList& commands, const RHI::Pipeline& pipeline, const RHI::BindGroup& group,
                 const MeshRenderer::RayShadowView& view, const Settings& settings) const;

        RHI::Device& m_device;
        std::unique_ptr<RHI::BindGroupLayout> m_layout;
        std::unique_ptr<RHI::Sampler> m_pointSampler;
        std::unique_ptr<RHI::Pipeline> m_compute;
        std::unique_ptr<RHI::Pipeline> m_denoise;
        std::unique_ptr<RHI::BindGroup> m_computeGroup;
        std::unique_ptr<RHI::BindGroup> m_denoiseGroup;
        RHI::Extent m_extent{};
        AmbientOcclusionQuality m_quality = DefaultAmbientOcclusionQuality;
    };
}
