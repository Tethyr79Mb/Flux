#pragma once

#include "Flux/Export.hpp"
#include "Flux/Renderer/RHI/RHI.hpp"
#include "Renderer/Exposure.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace Flux {
    // Exposition automatique : le GPU mesure la lumière de la scène (histogramme pondéré au centre,
    // shaders/Exposure.slang), le CPU relit la mesure FramesInFlight images plus tard (sans attendre le GPU) et en tire
    // l'exposition (Exposure.hpp). Deux images de retard, invisibles devant une adaptation de l'ordre de la seconde ;
    // en échange, l'exposition est connue du CPU (couleur de fond, panneau Moteur) et rien n'est lu et écrit par le
    // GPU d'une image à l'autre.
    // Coût : deux petits compute shaders (une lecture par bloc de 2 x 2 pixels), une copie de 512 octets. Configuration
    // minimum : la même chose.
    class FLUX_API ExposureRenderer { // exporté pour les tests (Tests/src/ExposureGpuTests.cpp)
        public:
        static std::expected<std::unique_ptr<ExposureRenderer>, std::string> Create(RHI::Device& device);

        // Quand le graphe de rendu a recréé l'image de la scène (le GPU déjà attendu).
        void Bind(const RHI::Texture& scene);
        // Après Device::BeginFrame : la mesure faite à la dernière image de même indice, s'il y en a une, rapproche
        // l'exposition de celle qu'elle demande. `seconds` depuis l'image précédente ; `snap` : aller tout de suite à
        // l'exposition visée (caméra qui saute, scène chargée). Rend l'exposition de cette image.
        float Update(const Exposure::Settings& settings, float seconds, bool snap, std::uint32_t frameIndex);
        // Hors de toute passe, la scène lisible par les compute shaders : la mesure de cette image.
        void Measure(RHI::CommandList& commands, std::uint32_t frameIndex);

        float GetExposure() const { return m_exposure; }
        // Dernière luminance moyenne mesurée (0 : rien de mesurable, image noire).
        float GetLuminance() const { return m_luminance; }

        private:
        explicit ExposureRenderer(RHI::Device& device) : m_device(device) {}

        RHI::Device& m_device;
        std::unique_ptr<RHI::Sampler> m_sampler;
        std::unique_ptr<RHI::BindGroupLayout> m_layout;
        std::unique_ptr<RHI::BindGroup> m_group;
        std::unique_ptr<RHI::Pipeline> m_clear;
        std::unique_ptr<RHI::Pipeline> m_histogram;
        std::unique_ptr<RHI::Buffer> m_bins; // écrit par les compute shaders
        // Copie de la mesure de chaque image en vol, relue par le CPU quand cette image revient.
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_readback;
        std::array<bool, RHI::FramesInFlight> m_measured{};
        RHI::Extent m_extent; // de la scène
        Exposure::Histogram m_read{};
        float m_exposure = 1.0f;
        float m_luminance = 0.0f;
        bool m_adapted = false; // une mesure déjà suivie : sinon, la première est prise telle quelle
    };
}
