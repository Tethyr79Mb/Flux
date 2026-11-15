#pragma once

#include "Flux/Renderer/GBufferView.hpp"
#include "Flux/Renderer/RHI/RHI.hpp"

#include <expected>
#include <memory>
#include <string>

namespace Flux {
    class MeshRenderer;

    // Éclairage différé des modèles opaques (shaders/Lighting.slang) : la passe « G-buffer » a écrit leur surface (couleur
    // de base, normales, métal, rugosité, occlusion) et, dans l'image de la scène, leur lumière propre (émissif et
    // contour) ; Draw éclaire chaque pixel où un modèle a été écrit, en compute, avec le même code que le rendu direct
    // (LightSurface de Mesh.slang : soleil et ombres, ciel, volumes intérieurs, lampes, air). Le sol, les arbres,
    // l'herbe, l'eau et les transparents restent en rendu direct, dessinés ensuite dans la même image.
    // Coût : un passage par pixel de la vue (8 x 8 par groupe), les pixels sans modèle quittent aussitôt ; les lampes
    // sont toutes parcourues par chaque pixel éclairé, comme en rendu direct (pas encore de tuiles).
    class LightingRenderer {
        public:
        // Formats des textures du G-buffer (16 octets par pixel) : couleur de base (sRGB) et sorte de matière en alpha
        // (GBufferEmpty : rien d'écrit) ; normale de la carte puis de la face (octaédriques, 16 bits) ; métal, rugosité,
        // occlusion et une réserve.
        static constexpr RHI::Format BaseFormat = RHI::Format::RGBA8Srgb;
        static constexpr RHI::Format NormalFormat = RHI::Format::RGBA16Unorm;
        static constexpr RHI::Format SurfaceFormat = RHI::Format::RGBA8Unorm;

        // Les données de l'image (groupes 0 à 2) sont celles de `meshes`, qui doit vivre plus longtemps.
        static std::expected<std::unique_ptr<LightingRenderer>, std::string> Create(RHI::Device& device, const MeshRenderer& meshes);

        // Textures du graphe de rendu, à la taille de rendu. sun : ray tracing seulement (lumière du soleil à part).
        struct Targets
        {
            const RHI::Texture* depth = nullptr;
            const RHI::Texture* base = nullptr;
            const RHI::Texture* normals = nullptr;
            const RHI::Texture* surface = nullptr;
            const RHI::Texture* color = nullptr; // l'image de la scène, lue (émissif et contour) puis écrite
            const RHI::Texture* sun = nullptr;
        };
        // Après chaque changement de taille des textures (GPU à l'arrêt).
        void Bind(const Targets& targets);

        // Hors d'une passe : le graphe a mis les barrières (image de la scène et soleil en Storage, le reste lu).
        // nearPlane : celui de la caméra (profondeur = proche / distance devant elle).
        void Draw(RHI::CommandList& commands, std::uint32_t frameIndex, float nearPlane) const;

        // Vue de débogage : à la place de l'éclairage des modèles, une donnée du G-buffer.
        void SetView(GBufferView view) { m_view = view; }
        GBufferView GetView() const { return m_view; }

        private:
        LightingRenderer(RHI::Device& device, const MeshRenderer& meshes) : m_device(device), m_meshes(meshes) {}

        RHI::Device& m_device;
        const MeshRenderer& m_meshes;
        std::unique_ptr<RHI::BindGroupLayout> m_layout;
        std::unique_ptr<RHI::Sampler> m_pointSampler;
        std::unique_ptr<RHI::Pipeline> m_lighting;
        std::unique_ptr<RHI::BindGroup> m_group;
        RHI::Extent m_extent{};
        bool m_rayTracing = false;
        GBufferView m_view = GBufferView::None;
    };
}
