#pragma once

#include "Renderer/RHI/RHI.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace Flux::Planetology {
    // Textures 3D de bruit des nuages, calculées une fois par le GPU (shaders/CloudNoise.slang) avec tous leurs niveaux
    // de mip, puis lues par le rendu des nuages : la forme (128³, Perlin-Worley et trois Worley) et le détail qui ronge
    // les bords (32³, trois Worley). Elles se répètent sans couture : le rendu les lit à l'échelle qu'il veut.
    // Mémoire vidéo : 9,7 Mo pour les deux, mips compris.
    class FLUX_API CloudNoise {
        public:
        static constexpr std::uint32_t ShapeSize = 128;
        static constexpr std::uint32_t DetailSize = 32;

        // Calcule les deux textures et attend le GPU (Device::Submit) : au démarrage seulement, quelques
        // millisecondes sur une carte graphique.
        static std::expected<CloudNoise, std::string> Create(RHI::Device& device);

        const RHI::Texture& GetShape() const { return *m_shape; }
        const RHI::Texture& GetDetail() const { return *m_detail; }

        private:
        CloudNoise() = default;

        std::unique_ptr<RHI::Texture> m_shape;
        std::unique_ptr<RHI::Texture> m_detail;
    };
}
