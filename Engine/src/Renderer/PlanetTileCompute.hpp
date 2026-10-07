#pragma once

#include "Flux/Export.hpp"
#include "Flux/Graphics/Planet.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Flux {
    // Tuiles des planètes calculées par le GPU (shaders/PlanetTile.slang) : chaque image, les tuiles ajoutées par Add
    // sont calculées par Record, avant les passes de dessin, dans leurs buffers de sommets. Leur sphère englobante
    // et leur point le plus bas reviennent au CPU quand le GPU a fini l'image (GetStats).
    // Exporté pour les tests (Tests/src/PlanetGpuTests.cpp).
    class FLUX_API PlanetTileCompute {
        public:
        // Tuiles calculées par image au plus : quelques dizaines de microsecondes de GPU en tout.
        static constexpr std::uint32_t MaxTilesPerFrame = 64;

        static std::expected<std::unique_ptr<PlanetTileCompute>, std::string> Create(RHI::Device& device);
        ~PlanetTileCompute();
        PlanetTileCompute(const PlanetTileCompute&) = delete;
        PlanetTileCompute& operator=(const PlanetTileCompute&) = delete;

        // Sommets d'une tuile (ModelVertex et Planet::MorphVertex, Planet::TileVertexCount chacun), écrits par le GPU
        // et lus comme sommets. memory : Readback pour les relire sur le CPU (tests).
        struct TileBuffers
        {
            std::unique_ptr<RHI::Buffer> vertices;
            std::unique_ptr<RHI::Buffer> morph;
            std::unique_ptr<RHI::BindGroup> group;
        };
        std::expected<TileBuffers, std::string> CreateTileBuffers(RHI::MemoryType memory = RHI::MemoryType::GpuOnly);

        struct Stats
        {
            float lowest; // plus petite distance d'un sommet au centre de la planète (forme de la parente comprise)
            float radius; // sphère englobante autour de Planet::TileMesh::center, jupes et glissement compris
            float highest; // plus grande distance d'un sommet au centre de la planète (sans les jupes)
            float unused;
        };
        // Tuiles calculées la dernière fois que l'image `frameIndex` a servi, dans l'ordre des Add ; à lire une fois
        // le GPU passé (après Device::BeginFrame revenu sur cette image, ou Device::Submit).
        std::span<const Stats> GetStats(std::uint32_t frameIndex);
        // false si l'image a déjà MaxTilesPerFrame tuiles. Les buffers doivent vivre jusqu'à la fin de l'image.
        bool Add(std::uint32_t frameIndex, const Planet::Terrain& terrain, Planet::TileKey key, const TileBuffers& buffers);
        // Calcule les tuiles ajoutées pour cette image, hors d'une passe ; lisibles par les commandes suivantes.
        void Record(RHI::CommandList& commands, std::uint32_t frameIndex);

        private:
        explicit PlanetTileCompute(RHI::Device& device) : m_device(device) {}

        struct Frame
        {
            std::unique_ptr<RHI::Buffer> inputs; // Planet::TileGpuInputs, écrit par le CPU
            std::unique_ptr<RHI::Buffer> stats;  // Stats, relu par le CPU
            std::unique_ptr<RHI::BindGroup> group;
            std::vector<const RHI::BindGroup*> tiles; // ajoutées, pas encore calculées
            std::uint32_t computed = 0;               // calculées la dernière fois
            std::vector<Stats> readStats;
        };

        RHI::Device& m_device;
        std::unique_ptr<RHI::BindGroupLayout> m_frameLayout;
        std::unique_ptr<RHI::BindGroupLayout> m_tileLayout;
        std::unique_ptr<RHI::Pipeline> m_pipeline;
        std::array<Frame, RHI::FramesInFlight> m_frames;
        Planet::TileGpuInputs m_inputs; // 2,5 Ko : pas sur la pile
    };
}
