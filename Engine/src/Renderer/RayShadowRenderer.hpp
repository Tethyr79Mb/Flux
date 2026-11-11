#pragma once

#include "Flux/Renderer/RHI/RHI.hpp"
#include "Renderer/MeshRenderer.hpp"

#include <array>
#include <expected>
#include <memory>
#include <string>

namespace Flux {
    // Ombres du soleil des modèles fixes en rayons (shaders/RayShadows.slang), après la passe de la scène, quand le GPU a
    // le ray tracing et que MeshRenderer::AreRayShadowsActive : un rayon par pixel qui reçoit du soleil (Trace), un
    // lissage selon la largeur de la pénombre, sur les lignes puis les colonnes (FilterRows, FilterColumns), puis la
    // part de la lumière du soleil qui arrive ajoutée à l'image de la scène, qui ne l'avait pas (Composite). Les
    // rayons hors du shader d'éclairage : il garde son coût, et seuls les pixels éclairés en lancent.
    class RayShadowRenderer {
        public:
        // `sceneFormat` : celui de l'image de la scène (Composite y dessine).
        static std::expected<std::unique_ptr<RayShadowRenderer>, std::string> Create(RHI::Device& device, RHI::Format sceneFormat);

        // Format des textures de travail (visibilité, distance de l'obstacle), écrites par les compute shaders.
        static constexpr RHI::Format WorkFormat = RHI::Format::RG16Float;
        struct Targets
        {
            const RHI::Texture* depth = nullptr; // profondeur de la scène
            const RHI::Texture* sun = nullptr;   // lumière du soleil à part (MeshRenderer::SunFormat)
            const RHI::Texture* rays = nullptr;  // trois textures de travail (WorkFormat) : rayons, lignes, colonnes
            const RHI::Texture* rows = nullptr;
            const RHI::Texture* mask = nullptr;
        };
        // Après chaque changement de taille des textures (GPU à l'arrêt).
        void Bind(const Targets& targets);

        // Hors d'une passe, chacun dans la sienne (le graphe de rendu met les barrières entre elles).
        void Trace(RHI::CommandList& commands, std::uint32_t frameIndex, const RHI::AccelerationStructure& scene,
                   const MeshRenderer::RayShadowView& view);
        void FilterRows(RHI::CommandList& commands, const MeshRenderer::RayShadowView& view) const;
        void FilterColumns(RHI::CommandList& commands, const MeshRenderer::RayShadowView& view) const;
        // Dans une passe vers l'image de la scène (gardée), viewport compris.
        void Composite(RHI::CommandList& commands) const;

        // Vue de débogage : à la place de la scène, rouge où le rayon est arrêté (avant le lissage), vert selon la
        // lumière du soleil reçue ; vert seul : soleil reçu que rien n'arrête.
        void SetShowMask(bool enabled) { m_showMask = enabled; }
        bool IsShowingMask() const { return m_showMask; }

        private:
        explicit RayShadowRenderer(RHI::Device& device) : m_device(device) {}
        void Run(RHI::CommandList& commands, const RHI::Pipeline& pipeline, const RHI::BindGroup& group,
                 const MeshRenderer::RayShadowView& view, std::uint32_t direction) const;

        RHI::Device& m_device;
        std::unique_ptr<RHI::BindGroupLayout> m_layout;
        std::unique_ptr<RHI::Sampler> m_pointSampler;
        std::unique_ptr<RHI::Pipeline> m_trace;
        std::unique_ptr<RHI::Pipeline> m_filter;
        std::unique_ptr<RHI::Pipeline> m_composite;
        std::unique_ptr<RHI::Pipeline> m_maskView;
        // Rayons : un groupe par image en vol (la structure de la scène est celle de l'image).
        std::array<std::unique_ptr<RHI::BindGroup>, RHI::FramesInFlight> m_traceGroups;
        std::array<const RHI::AccelerationStructure*, RHI::FramesInFlight> m_boundScene{};
        std::unique_ptr<RHI::BindGroup> m_rowsGroup;
        std::unique_ptr<RHI::BindGroup> m_columnsGroup;
        std::unique_ptr<RHI::BindGroup> m_compositeGroup;
        std::unique_ptr<RHI::BindGroup> m_maskGroup; // rayons avant le lissage
        Targets m_targets;
        bool m_showMask = false;
    };
}
