#pragma once

#include "Flux/Export.hpp"
#include "Flux/Graphics/ShadowCascades.hpp"
#include "Flux/Math/Quaternion.hpp"
#include "Flux/Planetology/Planet.hpp"
#include "Flux/Scene/Components.hpp"
#include "Planetology/PlanetRenderer.hpp"
#include "Planetology/VegetationLibrary.hpp"
#include "Renderer/ModelCache.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Flux::Planetology {
    // Arbres d'une planète (VegetationComponent), entièrement sur le GPU.
    // Semis : la surface est découpée en plaines, les tuiles de PlanetRenderer d'un niveau fixe (GetPlainLevel, bord
    // d'environ 200 à 400 m). Chaque plaine en vue est semée une fois par le GPU (computeTrees dans
    // shaders/PlanetTile.slang) : 64 x 64 places, chacune gardée ou non selon le hasard de sa place (toujours le même),
    // les biomes (VegetationLibrary : zone du relief, altitude, climat, forêts), la pente, sur le relief le plus fin ;
    // le biome tire aussi l'espèce de l'arbre. Ses arbres restent en mémoire vidéo (MaxPlains plaines) tant qu'elle sert.
    // Chaque image : les arbres des plaines en vue sont triés par le GPU (shaders/Vegetation.slang) en listes : arbre
    // détaillé, simplifié, imposteur selon la distance, et une liste d'ombre par cascade (modèle simplifié), chacune
    // rangée par espèce. Chaque espèce d'une liste se dessine en un appel indirect par primitive de son modèle
    // (MeshRenderer) : le CPU ne voit aucun arbre.
    // Les plaines ne sont visitées par PlanetRenderer qu'à moins de 6 bords de tuile environ : au-delà, pas d'arbre.
    // Exporté pour les tests (Tests/src/PlanetGpuTests.cpp).
    class FLUX_API VegetationRenderer {
        public:
        // Doivent correspondre à shaders/PlanetTile.slang et shaders/Vegetation.slang.
        static constexpr std::uint32_t TreeGrid = 64;
        static constexpr std::uint32_t TreesPerPlain = TreeGrid * TreeGrid;
        // Plaines gardées semées (32 octets par place : 128 Ko par plaine).
        static constexpr std::uint32_t MaxPlains = 256;
        // Semées par image au plus : chacune coûte environ autant de relief que 8 tuiles de sol.
        static constexpr std::uint32_t MaxSownPerFrame = 4;
        static constexpr std::uint32_t MaxVisiblePlains = 256;
        static constexpr std::uint32_t MaxSpecies = VegetationSet::MaxSpecies;
        // Une par primitive de chaque espèce de chaque liste (13 pour une espèce à deux primitives).
        static constexpr std::uint32_t MaxCommands = 128;
        // Listes d'arbres de l'image.
        enum List : std::uint32_t { Near, Middle, Far, Shadow, ListCount = Shadow + ShadowCascadeCount };
        // Arbres au plus par liste, toutes espèces ensemble (au-delà, les suivants ne sont pas dessinés).
        static constexpr std::array<std::uint32_t, ListCount> Capacities{8192, 32768, 131072, 16384, 16384, 16384, 16384};

        // Niveau des tuiles qui servent de plaines : le premier dont le bord fait au plus 400 m.
        static std::uint32_t GetPlainLevel(const Terrain& terrain);

        // memory : Readback pour relire les arbres semés sur le CPU (tests).
        static std::expected<std::unique_ptr<VegetationRenderer>, std::string> Create(RHI::Device& device,
                                                                                       RHI::MemoryType memory = RHI::MemoryType::GpuOnly);
        ~VegetationRenderer();
        VegetationRenderer(const VegetationRenderer&) = delete;
        VegetationRenderer& operator=(const VegetationRenderer&) = delete;

        // Début d'une image, avant SetPlanet : le GPU a fini l'image précédente de même indice.
        void BeginFrame(std::uint32_t frameIndex);
        // La planète à arbres de l'image (une seule : les suivantes sont ignorées) et ses plaines en vue
        // (PlanetRenderer::Collect). rotation : celle de la planète, relative à la caméra.
        // biomes : ceux de vegetation (VegetationLibrary).
        void SetPlanet(Entity entity, const Terrain& terrain, const PlanetComponent& planet, const VegetationComponent& vegetation,
                       const VegetationSet& biomes, const Math::Quaternionf& rotation, std::span<const PlanetRenderer::Plain> plains);

        // Champ de la caméra, comme ViewCone (MeshRenderer.cpp).
        struct View
        {
            Math::Vector3f forward, right, up;
            float tanX, tanY;
        };
        // Les trois modèles d'une espèce (ModelCache) ; nullptr tant qu'un n'est pas chargé : les plaines sont semées,
        // l'espèce n'est pas dessinée.
        struct Models
        {
            const GpuModel* near = nullptr;
            const GpuModel* middle = nullptr;
            const GpuModel* far = nullptr;
        };
        // Fin de la préparation : plaines à semer, plaines à trier, commandes de dessin. models : de chaque espèce de
        // VegetationSet::species.
        void EndFrame(std::uint32_t frameIndex, const View& view, const std::array<ShadowCascade, ShadowCascadeCount>& cascades,
                      const ShadowReceivers& receivers, std::span<const Models> models);
        // Semis puis tri, hors d'une passe et avant les dessins ; séparés pour que le panneau Moteur mesure chacun.
        void Sow(RHI::CommandList& commands, std::uint32_t frameIndex);
        void Cull(RHI::CommandList& commands, std::uint32_t frameIndex);

        // Un appel de dessin : une primitive du modèle d'une espèce dans sa liste, sa commande dans GetCommands.
        struct Draw
        {
            const GpuModel* model;
            std::uint32_t primitive;
            std::uint32_t command;
        };
        std::span<const Draw> GetDraws(List list) const { return m_lists[list]; }
        const RHI::Buffer& GetCommands(std::uint32_t frameIndex) const { return *m_frames[frameIndex].commands; }
        const RHI::Buffer& GetInstances(std::uint32_t frameIndex) const { return *m_frames[frameIndex].instances; }

        struct Stats
        {
            std::array<std::uint32_t, ListCount> trees{}; // de chaque liste, deux images plus tôt
            std::uint32_t plains = 0;   // triées à la dernière image
            std::uint32_t stored = 0;   // semées, en mémoire
            std::uint32_t pending = 0;  // en vue, pas encore semées
            std::uint32_t sown = 0;     // semées à cette image (au plus MaxSownPerFrame)
        };
        Stats GetStats() const { return m_stats; }

        // Un arbre semé, tel que l'écrit computeTrees.
        struct Tree
        {
            Math::Vector3f position; // relative au point de référence de la tuile de sa plaine
            float scale;
            Math::Vector3f up;       // repère de la planète
            float speciesAngle;      // espèce (VegetationSet::species) x 8 + angle autour du haut (radians)
        };
        // Relit les arbres d'une plaine semée (mémoire Readback, le GPU passé) ; false si elle ne l'est pas.
        bool ReadTrees(TileKey key, std::vector<Tree>& trees) const;

        private:
        explicit VegetationRenderer(RHI::Device& device) : m_device(device) {}

        // Doit correspondre à TreeJob dans shaders/PlanetTile.slang.
        struct TreeJob
        {
            std::array<std::uint32_t, 4> cell;
            std::array<std::uint32_t, 4> options;
            std::array<float, 4> shape;
            std::array<float, 4> climate;
        };
        // Doit correspondre à SowVegetation dans shaders/PlanetTile.slang.
        struct SowVegetation
        {
            std::array<BiomeGpu, VegetationSet::MaxBiomes> biomes;
            std::array<std::array<std::uint32_t, 4>, VegetationSet::MaxBiomes> biomeSpecies;
            std::array<std::array<float, 4>, VegetationSet::MaxBiomes> biomeShares;
            std::array<std::array<float, 4>, MaxSpecies> species;
            std::array<std::uint32_t, 4> counts;
        };
        // Doit correspondre à CullCell dans shaders/Vegetation.slang.
        struct CullCell
        {
            std::array<float, 4> origin;
        };
        // Doit correspondre à CullFrame dans shaders/Vegetation.slang.
        struct CullFrame
        {
            std::array<float, 4> forward;
            std::array<float, 4> right;
            std::array<float, 4> up;
            std::array<float, 4> distances;
            std::array<std::array<float, 4>, MaxSpecies> spheres;
            std::array<float, MaxSpecies> tints;
            std::array<float, 4> rotation;
            std::array<Math::Matrix4f, ShadowCascadeCount> cascades;
            std::array<std::array<float, 4>, ShadowCascadeCount> cascadeLimits;
            std::array<std::array<float, 4>, 4> receiverSides;
            std::array<float, 4> toSun;
            std::array<std::uint32_t, 8> firstInstances;
            std::array<std::uint32_t, 8> capacities;
            std::array<std::uint32_t, MaxCommands> commandLists; // liste x MaxSpecies + espèce
            std::array<std::uint32_t, 4> counts;
        };
        struct Frame
        {
            std::unique_ptr<RHI::Buffer> inputs;    // TileGpuInputs des plaines à semer
            std::unique_ptr<RHI::Buffer> jobs;      // TreeJob
            std::unique_ptr<RHI::Buffer> vegetation; // SowVegetation
            std::unique_ptr<RHI::Buffer> cullFrame; // CullFrame
            std::unique_ptr<RHI::Buffer> cells;     // CullCell
            std::unique_ptr<RHI::Buffer> commands;  // commandes, puis les compteurs (CounterOffset dans le .cpp)
            std::unique_ptr<RHI::Buffer> staged;    // arbres retenus, dans l'ordre du tri
            std::unique_ptr<RHI::Buffer> instances; // les mêmes rangés par espèce (vertexTree)
            std::unique_ptr<RHI::Buffer> stats;     // Readback : arbres de chaque liste
            std::unique_ptr<RHI::BindGroup> sowGroup;
            std::unique_ptr<RHI::BindGroup> cullGroup;
            std::uint32_t sown = 0;    // plaines à semer
            std::uint32_t culled = 0;  // plaines à trier
            bool counted = false;      // stats écrit par cette image
        };
        // Une plaine semée : sa case et la dernière image où elle a servi.
        struct StoredPlain
        {
            std::uint32_t slot;
            std::uint64_t lastUsed;
        };
        struct FreeSlot
        {
            std::uint32_t slot;
            std::uint64_t frame; // libérée à cette image : réutilisable quand le GPU ne la lit plus
        };

        std::uint32_t TakeSlot();
        void Forget();

        RHI::Device& m_device;
        std::unique_ptr<RHI::BindGroupLayout> m_sowLayout;
        std::unique_ptr<RHI::BindGroupLayout> m_cullLayout;
        std::unique_ptr<RHI::Pipeline> m_sow;
        std::unique_ptr<RHI::Pipeline> m_cull;
        std::unique_ptr<RHI::Pipeline> m_finalize;
        std::unique_ptr<RHI::Pipeline> m_scatter;
        std::unique_ptr<RHI::Buffer> m_trees;      // TreesPerPlain places par case, 2 float4 chacune
        std::unique_ptr<RHI::Buffer> m_treeCounts; // un par case
        std::array<Frame, RHI::FramesInFlight> m_frames;

        // Planète de l'image et ce dont dépend son semis : s'il change, tout est à resemer.
        Entity m_entity = NullEntity;     // de l'image (NullEntity : aucune)
        Entity m_sownPlanet = NullEntity; // celle des plaines semées en mémoire
        const Terrain* m_terrain = nullptr;
        TreeJob m_job{};
        SowVegetation m_sowVegetation{};
        std::array<float, MaxSpecies> m_tints{}; // TreeSpecies::tint de chaque espèce
        std::array<float, 10> m_signature{};
        Math::Quaternionf m_rotation;
        float m_lodDistance = 0.0f, m_farDistance = 0.0f, m_range = 0.0f;
        std::vector<PlanetRenderer::Plain> m_visible;

        std::unordered_map<std::uint64_t, StoredPlain> m_stored; // par TileKey::Pack
        std::vector<FreeSlot> m_freeSlots;
        std::uint32_t m_slotCount = 0;
        std::uint64_t m_frame = 0;
        std::array<std::vector<Draw>, ListCount> m_lists;
        std::vector<RHI::DrawIndexedCommand> m_commands;
        std::vector<CullCell> m_cells;
        std::vector<TreeJob> m_jobs;
        TileGpuInputs m_inputs; // 2,5 Ko : pas sur la pile
        Stats m_stats;
    };
}
