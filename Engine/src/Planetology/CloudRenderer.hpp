#pragma once

#include "Planetology/CloudNoise.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace Flux {
    class MeshRenderer;
}

namespace Flux::Planetology {
    // Nuages volumétriques des planètes (CloudsComponent), en trois passes après la scène opaque (shaders/Clouds.slang) :
    // la marche dans la couche à la moitié de la résolution (DrawMarch), le mélange à l'historique (DrawResolve), puis
    // la composition dans l'image de la scène à pleine résolution (DrawComposite), avant les transparents. Les
    // textures de travail sont au graphe de rendu (Targets) ; ici, le bruit 3D (CloudNoise), les pipelines et les
    // groupes qui les désignent. Les données de l'image (planètes, soleil, air) sont celles de MeshRenderer.
    // Coût : la marche, jusqu'à 64 pas par pixel (6 de plus vers le soleil là où il y a du nuage), au quart du nombre
    // de pixels de la vue ; puis deux passes plein écran légères. Mémoire vidéo : 9,7 Mo de bruit, et 18 Mo de
    // textures de travail en 1440p.
    class CloudRenderer {
        public:
        // Formats des textures de travail (demi-résolution) : la lumière des nuages (prémultipliée) et leur opacité ;
        // les distances (premier nuage, nuage moyen, fin du rayon), en float 32 bits pour les centaines de km.
        static constexpr RHI::Format LightFormat = RHI::Format::RGBA16Float;
        static constexpr RHI::Format DistanceFormat = RHI::Format::RGBA32Float;

        // `sceneFormat` : celui de l'image de la scène (DrawComposite y dessine) ; `reactiveFormat` : celui du masque
        // réactif de FSR (DrawReactive).
        static std::expected<std::unique_ptr<CloudRenderer>, std::string> Create(RHI::Device& device, const MeshRenderer& meshes,
                                                                                 RHI::Format sceneFormat,
                                                                                 RHI::Format reactiveFormat);

        // Textures du graphe de rendu : la profondeur de la scène (pleine résolution), les autres en demi-résolution.
        struct Targets
        {
            const RHI::Texture* sceneDepth = nullptr;
            const RHI::Texture* light = nullptr;
            const RHI::Texture* distances = nullptr;
            std::array<const RHI::Texture*, 2> history{}; // les deux exemplaires de l'historique
        };
        // Quand le graphe les a recréées (le GPU déjà attendu) : refait les groupes, oublie l'historique.
        void Bind(const Targets& targets);
        // L'historique ne vaut plus rien (image sans nuages, saut de la caméra) : la prochaine image repart de zéro.
        void ResetHistory() { m_historyValid = false; }

        // Chacune dans une passe de dessin déjà commencée, viewport compris : DrawMarch vers `light` et `distances`,
        // DrawResolve vers l'exemplaire `target` de l'historique (RenderGraph::GetWriteCopy, l'autre est lu),
        // DrawComposite vers l'image de la scène (gardée), qui lit l'exemplaire `target` écrit à cette image.
        void DrawMarch(RHI::CommandList& commands, std::uint32_t frameIndex) const;
        void DrawResolve(RHI::CommandList& commands, std::uint32_t frameIndex, std::uint32_t target);
        void DrawComposite(RHI::CommandList& commands, std::uint32_t frameIndex, std::uint32_t target) const;
        // En FSR, après DrawComposite, vers le masque réactif (gardé) : l'opacité des nuages qui y ont été mêlés.
        void DrawReactive(RHI::CommandList& commands, std::uint32_t frameIndex, std::uint32_t target) const;

        private:
        CloudRenderer(RHI::Device& device, const MeshRenderer& meshes, CloudNoise noise)
            : m_device(device), m_meshes(meshes), m_noise(std::move(noise)) {}
        void Draw(RHI::CommandList& commands, std::uint32_t frameIndex, const RHI::Pipeline& pipeline, const RHI::BindGroup& group,
                  float historyValid) const;

        RHI::Device& m_device;
        const MeshRenderer& m_meshes;
        CloudNoise m_noise;
        std::unique_ptr<RHI::Sampler> m_noiseSampler;  // linéaire, répété, avec les mips
        std::unique_ptr<RHI::Sampler> m_linearSampler; // demi-résolution : bilinéaire, bords bloqués
        std::unique_ptr<RHI::Sampler> m_pointSampler;  // profondeur : lue telle quelle
        std::unique_ptr<RHI::BindGroupLayout> m_layout; // groupe 3 de Clouds.slang
        std::unique_ptr<RHI::Pipeline> m_march;
        std::unique_ptr<RHI::Pipeline> m_resolve;
        std::unique_ptr<RHI::Pipeline> m_composite;
        std::unique_ptr<RHI::Pipeline> m_reactive; // mêmes groupes que la composition
        Targets m_targets;
        std::unique_ptr<RHI::BindGroup> m_marchGroup;
        std::array<std::unique_ptr<RHI::BindGroup>, 2> m_resolveGroups;   // [i] : écrit history[i], lit l'autre
        std::array<std::unique_ptr<RHI::BindGroup>, 2> m_compositeGroups; // [i] : lit history[i]
        bool m_historyValid = false;
    };
}
