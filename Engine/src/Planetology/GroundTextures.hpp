#pragma once

#include "Renderer/ModelCache.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace Flux::Planetology {
    // Matières du sol des planètes : une couche de chaque tableau de textures, dans cet ordre (fragmentPlanet dans
    // shaders/Mesh.slang).
    enum class GroundLayer : std::uint32_t { Rock, Grass, Soil, Sand, Snow, Count };

    // Images du sol des planètes, lues dans le dossier Textures/Sol des assets : pour chaque matière (roche, herbe,
    // terre, sable, neige), <nom>-couleur, <nom>-normales (OpenGL : vert vers le haut), <nom>-rugosite et, si elle
    // existe, <nom>-occlusion (.png ou .jpg, toutes de la même taille, carrées). Regroupées en deux tableaux de
    // textures (couleur et rugosité en alpha ; normales en rouge-vert et occlusion en bleu), liés au groupe 0 des tuiles de planète
    // (ModelCache::GroundColorSlot et suivants).
    // Sans ces images, les planètes gardent leurs couleurs unies.
    // Contre la répétition, chaque zone du sol lit l'image tournée au hasard. Le fichier matieres.json du même dossier
    // borne cet angle par matière, en degrés : {"rotation": {"sable": 10, ...}}. 180 : rotation libre (roche, herbe,
    // terre) ; peu : l'image garde son sens (rides du sable, congères). L'angle suit l'image, pas la planète. Sans le
    // fichier ou sans une matière : DefaultMaxAngles.
    // Les images sont décodées sur d'autres fils (une vingtaine de JPEG de 2048² : plusieurs secondes sur un seul) :
    // ni le démarrage ni le jeu ne les attendent, les planètes gardent leurs couleurs unies jusque-là.
    class GroundTextures {
        public:
        GroundTextures() = default;
        ~GroundTextures();
        GroundTextures(const GroundTextures&) = delete;
        GroundTextures& operator=(const GroundTextures&) = delete;

        // Le groupe des tableaux, ou nullptr si les images manquent, sont illisibles ou pas encore décodées. Relit
        // les images quand le dossier des assets change ou qu'une image est modifiée (vérifié toutes les deux
        // secondes).
        const RHI::BindGroup* Get(RHI::Device& device, const ModelCache& models, const std::filesystem::path& assetsDirectory);
        // Images en cours de décodage (pas encore sur le GPU).
        bool IsLoading() const { return m_decoding.valid(); }
        // Rotation maximale de chaque matière (radians, dans l'ordre de GroundLayer ; FrameData::groundRotations),
        // relue avec les images quand matieres.json change (sans redécoder les images).
        const std::array<float, static_cast<std::size_t>(GroundLayer::Count)>& GetMaxAngles() const { return m_maxAngles; }
        // En degrés : sable et neige ±10°, le reste libre.
        static constexpr std::array<float, static_cast<std::size_t>(GroundLayer::Count)> DefaultMaxAngles{180.0f, 180.0f, 180.0f, 10.0f, 10.0f};

        // Pixels des deux tableaux, décodés hors du fil principal.
        struct Decoded
        {
            int size = 0;
            std::array<std::vector<std::uint8_t>, 2> pixels;
            std::string error; // non vide : rien à afficher
        };

        private:
        bool Upload(RHI::Device& device, const ModelCache& models, Decoded& decoded);
        std::filesystem::file_time_type NewestWriteTime() const;
        void ReadSettings();

        std::filesystem::path m_directory; // Textures/Sol
        std::filesystem::file_time_type m_writeTime = std::filesystem::file_time_type::min(); // aucune image
        std::chrono::steady_clock::time_point m_checked{};
        std::filesystem::file_time_type m_settingsTime = std::filesystem::file_time_type::min(); // matieres.json, min : absent
        std::array<float, static_cast<std::size_t>(GroundLayer::Count)> m_maxAngles{};
        std::future<Decoded> m_decoding; // valide : décodage en cours ou fini, pas encore envoyé au GPU
        std::shared_ptr<std::atomic<bool>> m_cancel; // celui du décodage en cours : arrête les images restantes
        // Dans cet ordre : le groupe est rendu avant les textures et le sampler qu'il référence.
        std::unique_ptr<RHI::Sampler> m_sampler;
        std::array<std::unique_ptr<RHI::Texture>, 2> m_textures; // couleur et rugosité, normales et occlusion
        std::unique_ptr<RHI::BindGroup> m_group;
    };
}
