#pragma once

#include "Flux/Math/Color.hpp"
#include "Flux/Math/Matrix4.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace Flux {
    // Image de la scène en HDR et ce qui la transforme en image de la fenêtre : anticrénelage temporel
    // (shaders/Temporal.slang), bloom puis courbe de tons (shaders/Post.slang). La scène est dessinée dans sa cible
    // (lumière linéaire, sans plafond), son mouvement propre dans la sienne, avec son propre tampon de profondeur, tous
    // trois à la taille de la vue 3D et créés par le graphe de rendu (Targets) ; Composite l'écrit ensuite dans la
    // passe principale. Seuls les niveaux du bloom sont créés ici.
    // Coût de l'anticrénelage : une passe plein écran (25 lectures de texture par pixel, la plupart dans le même
    // voisinage 3 x 3), et 40 Mo environ de mémoire vidéo en 1080p (deux historiques HDR et le mouvement).
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
        // Seconde cible de la passe de la scène : le mouvement propre de ce qu'on voit (voir Motion dans Mesh.slang).
        static constexpr RHI::Format MotionFormat = RHI::Format::RG16Float;

        // Textures du graphe de rendu, toutes à la taille de la vue 3D.
        struct Targets
        {
            const RHI::Texture* scene = nullptr;
            const RHI::Texture* depth = nullptr;
            const RHI::Texture* motion = nullptr;
            std::array<const RHI::Texture*, 2> history{}; // les deux exemplaires de l'historique
        };
        // Quand le graphe les a recréées (le GPU déjà attendu) : refait les niveaux du bloom et les groupes.
        std::expected<void, std::string> Bind(const Targets& targets);

        // Hors de toute passe, après la scène et avant DrawBloom : mélange la scène à l'historique (voir Temporal.slang).
        // `reprojection` : clip space de cette image sans décalage (x, y, profondeur, 1) -> celui de l'image précédente ;
        // `history` false : rien à reprendre (saut de la caméra). `target` : l'exemplaire de l'historique écrit
        // (RenderGraph::GetWriteCopy), l'autre est lu.
        void ResolveTemporal(RHI::CommandList& commands, std::uint32_t target, const Math::Matrix4f& reprojection, bool history);
        // Anticrénelage coupé à cette image : le bloom et l'image finale lisent la scène telle quelle.
        void SkipTemporal();

        // Hors de toute passe, après la scène : réductions et remontées du bloom. Rien si `strength` vaut 0. Les niveaux
        // sont internes (barrières posées ici) ; le résultat est laissé lisible pour Composite, même non dessiné.
        void DrawBloom(RHI::CommandList& commands, float strength);
        // Dans la passe principale, viewport sur la vue 3D : la scène et son bloom, avec la courbe de tons.
        void Composite(RHI::CommandList& commands, float strength, float exposure) const;

        // Doit correspondre à GridParams dans shaders/Grid.slang.
        struct GridParams
        {
            Math::Matrix4f inverseViewProjection; // clip sans décalage -> relatif à la caméra
            std::array<float, 4> depthRow;        // lignes z et w de la projection (relatif à la caméra -> clip)
            std::array<float, 4> wRow;
            std::array<float, 4> camera;          // x, z : caméra modulo 1000 m ; y : hauteur au-dessus du plan
            std::array<float, 4> axes;            // x, z : caméra (axes du monde) ; y : distance d'effacement
        };
        // Dans la passe principale, après Composite et avec le même viewport : la grille au sol de l'éditeur
        // (shaders/Grid.slang), cachée derrière la scène par sa profondeur. Un triangle plein écran, une lecture de
        // la profondeur par pixel.
        void DrawGrid(RHI::CommandList& commands, const GridParams& params) const;

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

        // Doit correspondre à TemporalParams dans shaders/Temporal.slang.
        struct TemporalParams
        {
            Math::Matrix4f reprojection;
            std::array<float, 4> texel;
        };

        static constexpr std::uint32_t MaxLevels = 6;
        // Image que lisent le bloom et l'image finale : la scène, ou l'un des deux historiques.
        static constexpr std::uint32_t OutputCount = 3;

        explicit PostRenderer(RHI::Device& device) : m_device(device) {}
        const RHI::Texture& GetOutput(std::uint32_t output) const;
        void DrawPass(RHI::CommandList& commands, const RHI::Pipeline& pipeline, const RHI::Texture& target,
                      const RHI::BindGroup& group, const RHI::Texture& source, float radius) const;

        RHI::Device& m_device;
        std::unique_ptr<RHI::Sampler> m_sampler;
        std::unique_ptr<RHI::BindGroupLayout> m_layout;
        std::unique_ptr<RHI::Pipeline> m_downsampleFirst;
        std::unique_ptr<RHI::Pipeline> m_downsample;
        std::unique_ptr<RHI::Pipeline> m_upsample;
        std::unique_ptr<RHI::Pipeline> m_composite;
        std::unique_ptr<RHI::Sampler> m_pointSampler; // profondeur et mouvement : lus tels quels
        std::unique_ptr<RHI::BindGroupLayout> m_temporalLayout;
        std::unique_ptr<RHI::Pipeline> m_temporal;
        std::unique_ptr<RHI::BindGroupLayout> m_gridLayout;
        std::unique_ptr<RHI::Pipeline> m_grid;
        std::unique_ptr<RHI::BindGroup> m_gridGroup; // la profondeur de la scène

        RHI::Extent m_extent;
        Targets m_targets;
        // Résultats de l'anticrénelage, en alternance : celui de cette image est écrit en lisant l'autre.
        std::array<std::unique_ptr<RHI::BindGroup>, 2> m_temporalGroups; // [i] : écrit history[i]
        bool m_historyValid = false;      // l'historique lu tient une image de cette taille
        bool m_historyCleared = false;    // les deux historiques ont déjà servi de cible (lisibles par un shader)
        std::uint32_t m_output = 0;       // 0 : la scène ; 1 + i : history[i]
        std::uint32_t m_levels = 0;
        // down[i] : la scène réduite i + 1 fois ; up[i] : down[i] plus tous les niveaux plus petits (up[m_levels - 1]
        // n'existe pas : c'est down[m_levels - 1]).
        std::array<std::unique_ptr<RHI::Texture>, MaxLevels> m_down;
        std::array<std::unique_ptr<RHI::Texture>, MaxLevels> m_up;
        // La première réduction lit l'image de sortie (m_output) : un groupe par sortie possible, comme l'image finale
        // (m_downGroups[0] n'est pas utilisé).
        std::array<std::unique_ptr<RHI::BindGroup>, MaxLevels> m_downGroups;
        std::array<std::unique_ptr<RHI::BindGroup>, MaxLevels> m_upGroups;
        std::array<std::unique_ptr<RHI::BindGroup>, OutputCount> m_firstDownGroups;
        std::array<std::unique_ptr<RHI::BindGroup>, OutputCount> m_compositeGroups;
        bool m_bloomDrawn = false; // up[0] (ou down[0]) rempli cette image : sinon Composite lit la scène deux fois
    };
}
