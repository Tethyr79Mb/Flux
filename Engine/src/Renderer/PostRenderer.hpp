#pragma once

#include "Flux/Math/Color.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace Flux {
    // Image de la scène en HDR et ce qui la transforme en image de la fenêtre : bloom puis courbe de tons
    // (shaders/Post.slang). La scène est dessinée dans GetSceneTarget (lumière linéaire, sans plafond) avec son propre
    // tampon de profondeur, tous deux à la taille de la vue 3D ; Composite l'écrit ensuite dans la passe principale.
    // Coût du bloom : une demi-douzaine de petites passes plein écran (la première à la moitié de la taille de la vue),
    // une quinzaine de lectures de texture par pixel, et 5 Mo environ de mémoire vidéo en 1080p.
    class PostRenderer {
        public:
        // `outputFormat` : celui de la fenêtre ; `depthFormat` : celui de la passe principale (Composite y dessine).
        static std::expected<std::unique_ptr<PostRenderer>, std::string> Create(RHI::Device& device, RHI::Format outputFormat,
                                                                                RHI::Format depthFormat);

        // Format de l'image de la scène : ce que les pipelines de la scène déclarent.
        static constexpr RHI::Format SceneFormat = RHI::Format::RGBA16Float;
        static constexpr RHI::Format DepthFormat = RHI::Format::D32Float;

        // Cibles à la taille de la vue 3D, recréées si elle change (le GPU attendu avant).
        std::expected<void, std::string> Resize(RHI::Extent extent);
        const RHI::Texture& GetSceneTarget() const { return *m_scene; }
        const RHI::Texture& GetSceneDepth() const { return *m_depth; }

        // Hors de toute passe, après la scène : réductions et remontées du bloom. Rien si `strength` vaut 0.
        void DrawBloom(RHI::CommandList& commands, float strength);
        // Dans la passe principale, viewport sur la vue 3D : la scène et son bloom, avec la courbe de tons.
        void Composite(RHI::CommandList& commands, float strength, float exposure) const;

        // Couleur HDR qui donne `color` après la courbe de tons : le fond d'une scène sans ciel garde la couleur
        // choisie, quelle que soit la courbe.
        static Math::LinearColor InverseToneMap(Math::LinearColor color, float exposure);

        private:
        // Doit correspondre à PostParams dans shaders/Post.slang.
        struct PostParams
        {
            std::array<float, 4> texel;
            std::array<float, 4> options;
        };

        static constexpr std::uint32_t MaxLevels = 6;

        explicit PostRenderer(RHI::Device& device) : m_device(device) {}
        void DrawPass(RHI::CommandList& commands, const RHI::Pipeline& pipeline, const RHI::Texture& target,
                      const RHI::BindGroup& group, const RHI::Texture& source, float radius) const;

        RHI::Device& m_device;
        std::unique_ptr<RHI::Sampler> m_sampler;
        std::unique_ptr<RHI::BindGroupLayout> m_layout;
        std::unique_ptr<RHI::Pipeline> m_downsampleFirst;
        std::unique_ptr<RHI::Pipeline> m_downsample;
        std::unique_ptr<RHI::Pipeline> m_upsample;
        std::unique_ptr<RHI::Pipeline> m_composite;

        RHI::Extent m_extent;
        std::unique_ptr<RHI::Texture> m_scene;
        std::unique_ptr<RHI::Texture> m_depth;
        std::uint32_t m_levels = 0;
        // down[i] : la scène réduite i + 1 fois ; up[i] : down[i] plus tous les niveaux plus petits (up[m_levels - 1]
        // n'existe pas : c'est down[m_levels - 1]).
        std::array<std::unique_ptr<RHI::Texture>, MaxLevels> m_down;
        std::array<std::unique_ptr<RHI::Texture>, MaxLevels> m_up;
        std::array<std::unique_ptr<RHI::BindGroup>, MaxLevels> m_downGroups;
        std::array<std::unique_ptr<RHI::BindGroup>, MaxLevels> m_upGroups;
        std::unique_ptr<RHI::BindGroup> m_compositeGroup;
        bool m_bloomDrawn = false; // up[0] (ou down[0]) rempli cette image : sinon Composite lit la scène deux fois
    };
}
