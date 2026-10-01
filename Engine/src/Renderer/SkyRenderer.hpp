#pragma once

#include "Flux/Graphics/Sky.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <map>
#include <vector>

namespace Flux {
    struct Camera;

    // Ce que le ciel donne à l'éclairage des maillages (MeshRenderer) pour une image.
    struct SkyLighting
    {
        Math::Vector3f toSun;       // unitaire
        Math::Vector3f sunColor;    // couleur x intensité
        float ambient = 1.0f;       // multiplie la texture cube (lumière ambiante et reflets)
        float exposure = 1.0f;
        // Texture cube du ciel (les ressources de l'image en préparation) et son nombre de niveaux de mip.
        const RHI::Texture* environment = nullptr;
        const RHI::Sampler* environmentSampler = nullptr;
        std::uint32_t environmentMips = 1;
    };

    // Ciel de la scène (SkyComponent) : le fond, dessiné après les opaques là où rien ne l'a été, et la texture cube
    // qui éclaire les maillages (lumière ambiante, reflets). Sans SkyComponent : pas de fond (la couleur d'effacement),
    // un dégradé gris pour la lumière ambiante et le soleil par défaut, l'éclairage d'avant le ciel.
    class SkyRenderer {
        public:
        static std::expected<std::unique_ptr<SkyRenderer>, std::string> Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                RHI::Format depthFormat);

        // Après Device::BeginFrame : les ressources de l'image frameIndex sont libres. `viewportHeight` en pixels.
        SkyLighting Prepare(const Scene& scene, const Camera& camera, float aspect, float viewportHeight,
                            const std::filesystem::path& assetsDirectory, std::uint32_t frameIndex);
        // Dans la passe principale, après les opaques et avant les transparents.
        void Draw(RHI::CommandList& commands, std::uint32_t frameIndex) const;

        private:
        // D'où vient la texture cube : quand elle change, la texture est refaite.
        struct EnvironmentKey
        {
            enum class Kind : std::uint8_t { Gradient, Generated, Panorama };
            Kind kind = Kind::Gradient;
            SkyParameters parameters; // Generated
            std::filesystem::path panorama; // Panorama

            bool operator==(const EnvironmentKey&) const = default;
        };

        // Image panoramique chargée : la texture du fond et sa texture cube (pixels), gardées jusqu'à la fermeture.
        struct Panorama
        {
            std::unique_ptr<RHI::Texture> texture; // nul : fichier illisible
            std::vector<std::uint8_t> cube;
        };

        explicit SkyRenderer(RHI::Device& device) : m_device(device) {}
        const Panorama& LoadPanorama(const std::filesystem::path& path);
        void UpdateEnvironment(const EnvironmentKey& key, std::uint32_t frameIndex);

        RHI::Device& m_device;
        std::unique_ptr<RHI::Pipeline> m_pipeline;
        std::unique_ptr<RHI::BindGroupLayout> m_layout;
        std::unique_ptr<RHI::Sampler> m_panoramaSampler;
        std::unique_ptr<RHI::Sampler> m_cubeSampler;
        std::unique_ptr<RHI::Texture> m_black; // à la place du panorama pour le ciel généré
        // Une par image en vol : les données du shader, le groupe et le panorama lié.
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_buffers;
        std::array<std::unique_ptr<RHI::BindGroup>, RHI::FramesInFlight> m_groups;
        std::array<const RHI::Texture*, RHI::FramesInFlight> m_boundPanorama{};
        bool m_visible = false; // un SkyComponent dans la scène : le fond est dessiné

        // Pixels de la texture cube voulue (refaits quand la clé change, au plus tous les MinRebuildInterval), puis
        // une texture par image en vol, refaite quand elle n'a pas la dernière version : jamais remplacée pendant que
        // le GPU la lit, sans attendre le GPU.
        EnvironmentKey m_key;
        bool m_hasPixels = false;
        std::vector<std::uint8_t> m_pixels;
        std::uint32_t m_pixelsSize = 0;
        std::uint64_t m_version = 0;
        std::chrono::steady_clock::time_point m_lastBuild;
        std::array<std::unique_ptr<RHI::Texture>, RHI::FramesInFlight> m_environments;
        std::array<std::uint64_t, RHI::FramesInFlight> m_environmentVersions{};

        std::map<std::filesystem::path, Panorama> m_panoramas;
    };
}
