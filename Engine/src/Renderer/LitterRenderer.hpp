#pragma once

#include "Flux/Renderer/RHI/RHI.hpp"

#include <expected>
#include <memory>
#include <string>

namespace Flux {
    class MeshRenderer;

    // Litière au pied des arbres de près des planètes (TreeSpecies::litter), en décalque projeté (shaders/Litter.slang) :
    // entre la passe « G-buffer » et l'éclairage, une boîte par arbre de la liste Near de VegetationRenderer (même
    // commande indirecte, mêmes instances) peint la couleur de base du G-buffer là où le sol, retrouvé depuis la
    // profondeur, est dans la boîte et tourné vers le haut. Elle suit donc la pente et les bosses du sol, sans maillage.
    // L'alpha du G-buffer (sorte de matière) est gardé (RHI::BlendMode::Decal) ; normales, rugosité et occlusion restent
    // celles du sol. Les brins d'herbe (surface.x non nul) ne sont pas peints.
    // Coût : les pixels des faces arrière de chaque boîte (une lecture de profondeur, de normale et de surface, une de
    // l'image), une fois par pixel même caméra dedans.
    class LitterRenderer {
        public:
        // Les données de l'image (groupes 0 à 2) et les arbres sont ceux de `meshes`, qui doit vivre plus longtemps.
        static std::expected<std::unique_ptr<LitterRenderer>, std::string> Create(RHI::Device& device, const MeshRenderer& meshes);

        // Textures du graphe de rendu, à la taille de rendu, lues par la passe.
        struct Targets
        {
            const RHI::Texture* depth = nullptr;
            const RHI::Texture* normals = nullptr;
            const RHI::Texture* surface = nullptr;
        };
        // Après chaque changement de taille des textures (GPU à l'arrêt).
        void Bind(const Targets& targets);

        // Dans une passe sur la couleur de base du G-buffer (gardée). nearPlane : celui de la caméra.
        void Draw(RHI::CommandList& commands, std::uint32_t frameIndex, float nearPlane) const;

        private:
        LitterRenderer(RHI::Device& device, const MeshRenderer& meshes) : m_device(device), m_meshes(meshes) {}

        RHI::Device& m_device;
        const MeshRenderer& m_meshes;
        std::unique_ptr<RHI::BindGroupLayout> m_layout;
        std::unique_ptr<RHI::Sampler> m_pointSampler;
        std::unique_ptr<RHI::Pipeline> m_pipeline;
        std::unique_ptr<RHI::BindGroup> m_group;
        std::unique_ptr<RHI::Buffer> m_indices; // une boîte : 8 coins (SV_VertexID), 12 triangles
    };
}
