#pragma once

#include "Flux/Graphics/Sky.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Planetology/AtmosphereGpu.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <map>
#include <optional>
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
        // Atmosphère de la planète la plus proche (aucune : mie[3] à 0).
        Planetology::AtmosphereGpu atmosphere;
        // Table de l'air entre la caméra et ce qu'elle voit (shaders/Atmosphere.slang), remplie par Bake quand la caméra
        // est dans l'air (atmosphere.scale[1]) ; jamais nulles.
        const RHI::Texture* airRayleigh = nullptr;
        const RHI::Texture* airMie = nullptr;
        const RHI::Sampler* airSampler = nullptr;
    };

    // Ciel de la scène (SkyComponent) : le fond, dessiné après les opaques là où rien ne l'a été, et la texture cube
    // qui éclaire les maillages (lumière ambiante, reflets). Sans SkyComponent : pas de fond (la couleur d'effacement),
    // un dégradé gris pour la lumière ambiante et le soleil par défaut, l'éclairage d'avant le ciel.
    // Près d'une planète à atmosphère (PlanetComponent), le fond montre aussi son air : ciel bleu vu du sol, halo vu de
    // l'espace (même sans SkyComponent, sur un fond noir).
    class SkyRenderer {
        public:
        // `motionFormat` : seconde cible de la passe de la scène (PostRenderer::MotionFormat), laissée telle quelle.
        static std::expected<std::unique_ptr<SkyRenderer>, std::string> Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                RHI::Format motionFormat, RHI::Format depthFormat);

        // Après Device::BeginFrame : les ressources de l'image frameIndex sont libres. `viewportHeight` en pixels.
        // Pas de décalage d'un sous-pixel (TemporalView::jitter), contrairement au reste de la scène : les étoiles, d'un
        // pixel, étalées par l'anticrénelage sur ceux d'à côté, pâliraient ; le fond, lui, est déjà lisse.
        SkyLighting Prepare(const Scene& scene, const Camera& camera, float aspect, float viewportHeight,
                            const std::filesystem::path& assetsDirectory, std::uint32_t frameIndex);
        // Hors de toute passe et avant la passe principale : calcule la nébuleuse du ciel généré dans sa texture cube si
        // Prepare l'a demandé (nouveau ciel, réglage changé), et les tables de l'air quand la caméra est dans l'air
        // d'une planète.
        void Bake(RHI::CommandList& commands, std::uint32_t frameIndex);
        // Dans la passe de la scène, après les opaques et avant les transparents : le fond puis le soleil.
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

        // Atmosphère de la planète la plus proche de la caméra, et sa lumière ambiante (refaite quand elle change).
        Planetology::AtmosphereGpu PrepareAtmosphere(const Scene& scene, const Camera& camera, const Math::Vector3f& sunColor);

        RHI::Device& m_device;
        std::optional<Planetology::AtmosphereParameters> m_ambientParameters;
        Math::Vector3f m_ambientSunColor;
        std::array<Math::Vector3f, Planetology::AtmosphereAmbientSize> m_ambientTable{};
        std::unique_ptr<RHI::Pipeline> m_pipeline;
        std::unique_ptr<RHI::Pipeline> m_sunPipeline; // shaders/Sun.slang, après le fond
        std::unique_ptr<RHI::BindGroupLayout> m_layout;
        std::unique_ptr<RHI::Sampler> m_panoramaSampler;
        std::unique_ptr<RHI::Sampler> m_cubeSampler;
        std::unique_ptr<RHI::Texture> m_black; // à la place du panorama pour le ciel généré
        // Une par image en vol : les données du shader, le groupe et le panorama lié.
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_buffers;
        std::array<std::unique_ptr<RHI::BindGroup>, RHI::FramesInFlight> m_groups;
        std::array<const RHI::Texture*, RHI::FramesInFlight> m_boundPanorama{};
        bool m_visible = false; // un SkyComponent dans la scène : le fond est dessiné

        // Fond du ciel généré (nébuleuse, bande galactique), calculé par le GPU dans m_background pour les réglages
        // m_bakedParameters ; refait quand ils changent, au plus tous les MinRebuildInterval (réglage qu'on glisse).
        // Jamais lu avant d'être calculé : m_blackCube à sa place.
        std::unique_ptr<RHI::Pipeline> m_bakePipeline;
        std::unique_ptr<RHI::Texture> m_background;
        std::unique_ptr<RHI::Texture> m_blackCube;
        std::optional<SkyParameters> m_bakedParameters;
        bool m_bakePending = false;
        std::chrono::steady_clock::time_point m_lastBake;
        std::array<const RHI::Texture*, RHI::FramesInFlight> m_boundBackground{};

        // Tables de l'air (shaders/Atmosphere.slang) : le ciel vu de la caméra et l'air jusqu'à ce qu'elle voit, deux
        // textures chacune. Une seule pour les deux images en vol : refaites en début d'image, lues dans la même image
        // (la passe attend que l'image d'avant ait fini de les lire). Remplies au moins une fois avant d'être lues.
        std::unique_ptr<RHI::Pipeline> m_skyTablePipeline;
        std::unique_ptr<RHI::Pipeline> m_airTablePipeline;
        std::unique_ptr<RHI::Texture> m_skyRayleigh;
        std::unique_ptr<RHI::Texture> m_skyMie;
        std::unique_ptr<RHI::Texture> m_airRayleigh;
        std::unique_ptr<RHI::Texture> m_airMie;
        bool m_tablesWanted = false; // caméra dans l'air cette image
        bool m_tablesFilled = false;

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
