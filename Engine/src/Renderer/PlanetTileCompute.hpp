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
    // sont calculées par Record, avant les passes de dessin, dans leur case des pages de sommets. Leur sphère
    // englobante et leur point le plus bas reviennent au CPU quand le GPU a fini l'image (GetStats).
    // Les sommets de toutes les tuiles sont rangés dans quelques grands buffers (pages de TilesPerPage cases) : une
    // page se dessine en un seul appel indirect (une commande par tuile, MeshRenderer), au lieu d'un appel et de
    // buffers par tuile.
    // Exporté pour les tests (Tests/src/PlanetGpuTests.cpp).
    class FLUX_API PlanetTileCompute {
        public:
        // Tuiles calculées par image au plus : quelques dizaines de microsecondes de GPU en tout.
        static constexpr std::uint32_t MaxTilesPerFrame = 64;
        // Cases par page : environ 17 Mo de sommets (ModelVertex et Planet::MorphVertex) par page.
        static constexpr std::uint32_t TilesPerPage = 256;
        // Pages au plus (8 192 tuiles, environ 550 Mo) : bien au-delà de ce que gardent quelques planètes.
        static constexpr std::uint32_t MaxPages = 32;

        // memory : Readback pour relire les sommets sur le CPU (tests).
        static std::expected<std::unique_ptr<PlanetTileCompute>, std::string> Create(RHI::Device& device,
                                                                                   RHI::MemoryType memory = RHI::MemoryType::GpuOnly);
        ~PlanetTileCompute();
        PlanetTileCompute(const PlanetTileCompute&) = delete;
        PlanetTileCompute& operator=(const PlanetTileCompute&) = delete;

        // Une case libre de plus (ajoute une page si besoin) ; les cases ne sont jamais rendues, l'appelant les réutilise.
        std::expected<std::uint32_t, std::string> CreateTile();
        static std::uint32_t GetPage(std::uint32_t tile) { return tile / TilesPerPage; }
        // Premier sommet de la case dans sa page (vertexOffset d'un dessin).
        static std::uint32_t GetFirstVertex(std::uint32_t tile) { return (tile % TilesPerPage) * Planet::TileVertexCount; }
        std::uint32_t GetPageCount() const { return static_cast<std::uint32_t>(m_pages.size()); }
        // Sommets de la page (ModelVertex et Planet::MorphVertex), écrits par le GPU et lus comme sommets.
        const RHI::Buffer& GetVertices(std::uint32_t page) const { return *m_pages[page].vertices; }
        const RHI::Buffer& GetMorph(std::uint32_t page) const { return *m_pages[page].morph; }
        // Relit les sommets d'une case (mémoire Readback, le GPU passé) : Planet::TileVertexCount de chaque.
        void ReadTile(std::uint32_t tile, ModelVertex* vertices, Planet::MorphVertex* morph) const;

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
        // false si l'image a déjà MaxTilesPerFrame tuiles. La case ne doit plus être lue par le GPU (images en vol).
        bool Add(std::uint32_t frameIndex, const Planet::Terrain& terrain, Planet::TileKey key, std::uint32_t tile);
        // Calcule les tuiles ajoutées pour cette image, hors d'une passe ; lisibles par les commandes suivantes.
        void Record(RHI::CommandList& commands, std::uint32_t frameIndex);

        private:
        PlanetTileCompute(RHI::Device& device, RHI::MemoryType memory) : m_device(device), m_memory(memory) {}

        struct Page
        {
            std::unique_ptr<RHI::Buffer> vertices;
            std::unique_ptr<RHI::Buffer> morph;
            std::unique_ptr<RHI::BindGroup> group; // les deux, écrits par le compute shader
        };
        struct Frame
        {
            std::unique_ptr<RHI::Buffer> inputs; // Planet::TileGpuInputs, écrit par le CPU
            std::unique_ptr<RHI::Buffer> stats;  // Stats, relu par le CPU
            std::unique_ptr<RHI::BindGroup> group;
            std::vector<std::uint32_t> tiles; // cases ajoutées, pas encore calculées
            std::uint32_t computed = 0;       // calculées la dernière fois
            std::vector<Stats> readStats;
        };

        RHI::Device& m_device;
        RHI::MemoryType m_memory;
        std::unique_ptr<RHI::BindGroupLayout> m_frameLayout;
        std::unique_ptr<RHI::BindGroupLayout> m_pageLayout;
        std::unique_ptr<RHI::Pipeline> m_pipeline;
        std::array<Frame, RHI::FramesInFlight> m_frames;
        std::vector<Page> m_pages;
        std::uint32_t m_tileCount = 0; // cases créées
        Planet::TileGpuInputs m_inputs; // 2,5 Ko : pas sur la pile
    };
}
