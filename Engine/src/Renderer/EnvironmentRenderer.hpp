#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Renderer/RHI/RHI.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace Flux {
    // D'où l'environnement tire sa lumière : le ciel de la scène (SkyRenderer::Prepare). Une source différente de la
    // précédente fait refaire l'environnement.
    struct EnvironmentSource
    {
        enum class Kind : std::uint8_t {
            Gradient,  // dégradé du bas (bottom) au haut (top) : scène sans ciel, ou panorama illisible (noir)
            Generated, // fond généré par le GPU (texture cube `background`)
            Panorama,  // image panoramique `panorama` (RGBA8 sRGB avec ses mips)
        };
        Kind kind = Kind::Gradient;
        const RHI::Texture* background = nullptr;
        const RHI::Texture* panorama = nullptr;
        Math::LinearColor top;
        Math::LinearColor bottom;
        // Change à chaque nouveau calcul de `background` : même texture, autre contenu.
        std::uint64_t revision = 0;

        bool operator==(const EnvironmentSource&) const = default;
    };

    // Lumière que le ciel donne aux maillages, calculée par le GPU (shaders/Environment.slang) et refaite seulement
    // quand le ciel change : reflets (texture cube HDR de 128 pixels, un niveau de mip par rugosité, préfiltré GGX) et
    // lumière diffuse (petite texture cube d'irradiance). Les deux restent les mêmes textures : les groupes qui les
    // lisent n'ont jamais à changer ; le graphe de rendu pose les barrières (écrites par Bake, lues par la scène).
    // Coût : quelques dixièmes de milliseconde à chaque changement du ciel, rien sinon. Configuration minimum : le même
    // calcul (trop rare pour mériter un repli).
    class FLUX_API EnvironmentRenderer { // exporté pour les tests (Tests/src/EnvironmentGpuTests.cpp)
        public:
        static constexpr RHI::Format Format = RHI::Format::RGBA16Float;

        static std::expected<std::unique_ptr<EnvironmentRenderer>, std::string> Create(RHI::Device& device);

        // Après Device::BeginFrame, chaque image où la scène est dessinée : la source de cette image.
        void Prepare(const EnvironmentSource& source, std::uint32_t frameIndex);
        // Environnement à refaire à cette image (source changée, ou jamais calculé).
        bool Pending() const { return m_pending; }
        // Hors de toute passe, après le calcul du fond du ciel et avant la scène, quand Pending ; reflets et irradiance
        // déjà cibles de rendu, la source lisible.
        void Bake(RHI::CommandList& commands, std::uint32_t frameIndex);

        const RHI::Texture& GetReflections() const { return *m_reflections; }
        const RHI::Texture& GetIrradiance() const { return *m_irradiance; }
        const RHI::Sampler& GetSampler() const { return *m_sampler; }
        std::uint32_t GetReflectionMips() const;

        private:
        explicit EnvironmentRenderer(RHI::Device& device) : m_device(device) {}

        RHI::Device& m_device;
        std::unique_ptr<RHI::Sampler> m_sampler;          // linéaire, bords des faces raccordés
        std::unique_ptr<RHI::Sampler> m_panoramaSampler;  // fait le tour en largeur
        std::unique_ptr<RHI::Texture> m_black;            // à la place du panorama
        std::unique_ptr<RHI::Texture> m_blackCube;        // à la place du fond généré
        std::unique_ptr<RHI::Texture> m_sky;              // le ciel en HDR, avec ses mips (interne)
        std::unique_ptr<RHI::Texture> m_reflections;
        std::unique_ptr<RHI::Texture> m_irradiance;
        std::unique_ptr<RHI::BindGroupLayout> m_inputLayout; // panorama et fond : changent avec la source
        std::unique_ptr<RHI::BindGroupLayout> m_skyLayout;   // m_sky
        // Un groupe des entrées par image en vol : on ne change pas celui que le GPU lit peut-être encore.
        std::array<std::unique_ptr<RHI::BindGroup>, RHI::FramesInFlight> m_inputGroups;
        std::array<EnvironmentSource, RHI::FramesInFlight> m_boundInputs;
        std::unique_ptr<RHI::BindGroup> m_skyGroup;
        std::unique_ptr<RHI::Pipeline> m_sourcePipeline;
        std::unique_ptr<RHI::Pipeline> m_prefilterPipeline;
        std::unique_ptr<RHI::Pipeline> m_irradiancePipeline;

        EnvironmentSource m_source;
        bool m_baked = false;
        bool m_pending = false;
    };
}
