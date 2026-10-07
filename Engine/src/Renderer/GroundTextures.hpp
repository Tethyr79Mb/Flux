#pragma once

#include "Renderer/ModelCache.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace Flux {
    // Matières du sol des planètes : une couche de chaque tableau de textures, dans cet ordre (fragmentPlanet dans
    // shaders/Mesh.slang).
    enum class GroundLayer : std::uint32_t { Rock, Grass, Soil, Sand, Snow, Count };

    // Images du sol des planètes, lues dans le dossier Textures/Sol des assets : pour chaque matière (roche, herbe,
    // terre, sable, neige), <nom>-couleur, <nom>-normales (OpenGL : vert vers le haut), <nom>-rugosite et, si elle
    // existe, <nom>-occlusion (.png ou .jpg, toutes de la même taille, carrées). Regroupées en trois tableaux de
    // textures (couleur ; normales ; occlusion en rouge et rugosité en vert), liés au groupe 0 des tuiles de planète
    // (ModelCache::GroundColorSlot et suivants).
    // Sans ces images, les planètes gardent leurs couleurs unies.
    class GroundTextures {
        public:
        // Le groupe des tableaux, ou nullptr si les images manquent ou sont illisibles. Recharge les images quand le
        // dossier des assets change ou qu'une image est modifiée (vérifié toutes les deux secondes).
        const RHI::BindGroup* Get(RHI::Device& device, const ModelCache& models, const std::filesystem::path& assetsDirectory);

        private:
        bool Load(RHI::Device& device, const ModelCache& models);
        std::filesystem::file_time_type NewestWriteTime() const;

        std::filesystem::path m_directory; // Textures/Sol
        std::filesystem::file_time_type m_writeTime = std::filesystem::file_time_type::min(); // aucune image
        std::chrono::steady_clock::time_point m_checked{};
        // Dans cet ordre : le groupe est rendu avant les textures et le sampler qu'il référence.
        std::unique_ptr<RHI::Sampler> m_sampler;
        std::array<std::unique_ptr<RHI::Texture>, 3> m_textures; // couleur, normales, occlusion-rugosité
        std::unique_ptr<RHI::BindGroup> m_group;
    };
}
