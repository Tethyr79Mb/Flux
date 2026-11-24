#pragma once

#include "Flux/Export.hpp"
#include "Flux/Planetology/OceanWaves.hpp"
#include "Flux/Renderer/RHI/RHI.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace Flux::Planetology {
    // Vagues de l'océan calculées par le GPU à chaque image (shaders/OceanWaves.slang) depuis le spectre de départ
    // (Flux/Planetology/OceanWaves.hpp) : par cascade, une texture du déplacement de l'eau (x, hauteur, z ; a : écume) et
    // une des pentes (d hauteur / dx, d hauteur / dz, dx / dx, dz / dz), 256², 16 bits flottants, avec leurs mips, qui
    // se répètent sur OceanCascadeLengths mètres.
    // Coût : 4 compute shaders par cascade (spectre, FFT des lignes, des colonnes, rangement), environ 0,1 ms par
    // cascade sur une carte de la configuration minimum. Mémoire vidéo : 1,4 Mo par cascade (textures et mips), 9 Mo de
    // spectre et de champs de travail. Configuration minimum : `cascades` à 2 (les vaguelettes de moins d'un mètre en moins).
    class FLUX_API OceanWavesGpu { // exporté pour les tests (Tests/src/OceanWavesGpuTests.cpp)
        public:
        static std::expected<std::unique_ptr<OceanWavesGpu>, std::string> Create(RHI::Device& device,
                                                                                 std::uint32_t cascades = OceanCascadeCount);

        // Spectre recalculé par le CPU (environ 10 ms) et envoyé, seulement s'il a changé. false : envoi impossible.
        bool SetSettings(const OceanWaveSettings& settings);
        // Hors de toute passe : la mer au temps `time` (s, depuis n'importe quand : ramené à OceanRepeatPeriod), après
        // `seconds` depuis l'appel d'avant (l'écume qui s'efface). Les textures finissent lisibles par les shaders.
        void Update(RHI::CommandList& commands, double time, float seconds);

        std::uint32_t GetCascadeCount() const { return m_cascades; }
        const RHI::Texture& GetDisplacement(std::uint32_t cascade) const { return *m_displacement[cascade]; }
        const RHI::Texture& GetSlopes(std::uint32_t cascade) const { return *m_slopes[cascade]; }
        const OceanWaveSettings& GetSettings() const { return m_settings; }
        bool HasSettings() const { return m_hasSettings; }
        // Écart type de la hauteur de l'eau (m) des cascades simulées, d'après le tirage.
        float GetHeightDeviation() const { return m_heightDeviation; }

        private:
        explicit OceanWavesGpu(RHI::Device& device) : m_device(device) {}

        RHI::Device& m_device;
        std::uint32_t m_cascades = 0;
        std::unique_ptr<RHI::Buffer> m_spectrum; // h0 de chaque cascade
        std::unique_ptr<RHI::Buffer> m_fields;   // champs complexes, transformés en place
        std::unique_ptr<RHI::BindGroupLayout> m_layout;
        std::array<std::unique_ptr<RHI::BindGroup>, OceanCascadeCount> m_groups;
        std::array<std::unique_ptr<RHI::Texture>, OceanCascadeCount> m_displacement;
        std::array<std::unique_ptr<RHI::Texture>, OceanCascadeCount> m_slopes;
        std::unique_ptr<RHI::Pipeline> m_spectrumPipeline;
        std::unique_ptr<RHI::Pipeline> m_fftPipeline;
        std::unique_ptr<RHI::Pipeline> m_assemblePipeline;
        OceanWaveSettings m_settings;
        bool m_hasSettings = false;
        float m_heightDeviation = 0.0f;
        bool m_hasFoam = false; // l'écume d'avant est dans les textures (sinon : contenu indéfini, pas relu)
    };
}
