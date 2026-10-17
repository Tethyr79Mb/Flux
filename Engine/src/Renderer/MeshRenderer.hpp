#pragma once

#include "Flux/Animation/Animation.hpp"
#include "Flux/Graphics/ShadowCascades.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Renderer/GroundTextures.hpp"
#include "Renderer/ModelCache.hpp"
#include "Renderer/PlanetRenderer.hpp"
#include "Renderer/RHI/RHI.hpp"
#include "Renderer/SkyRenderer.hpp"
#include "Renderer/TemporalView.hpp"
#include "Renderer/ThrusterRenderer.hpp"

#include <array>
#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Flux {
    struct Camera;
    class ViewCone;

    // Maillages de la scène (cubes et modèles glTF, animés ou non) : préparés sur le CPU pour chaque image
    // (matrices relatives à la caméra, poses des squelettes, cascades d'ombres), dessinés d'abord vus du soleil dans
    // la carte d'ombres (opaques seulement), puis dans la passe principale : les opaques d'abord, puis (après le fond
    // du ciel) les transparents (matériaux BLEND, ou teinte d'opacité < 1) de l'arrière vers l'avant. Éclairés par le
    // soleil et la texture cube du ciel (SkyRenderer).
    // Les tuiles des planètes (sol, eau, ombres) sont dessinées à part, par appels indirects : un par page de tuiles
    // (PlanetTileCompute) et par passe, plutôt qu'un appel par tuile.
    class MeshRenderer {
        public:
        // `motionFormat` : seconde cible de la passe de la scène, où les opaques écrivent leur mouvement propre
        // (PostRenderer::GetMotionTarget).
        static std::expected<std::unique_ptr<MeshRenderer>, std::string> Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                 RHI::Format motionFormat, RHI::Format depthFormat);

        ModelCache& GetModels() { return *m_models; }

        // Une matrice par maillage, calculée relativement à la caméra, les os des modèles animés et les cascades
        // d'ombres envoyés au GPU. `hidden` et ses descendants ne sont pas dessinés mais gardent leur ombre. Après
        // Device::BeginFrame : les ressources de l'image frameIndex sont libres.
        // Tuiles de planète de l'image préparée : dessinées, dont recouvertes d'eau, et gardées en mémoire.
        struct PlanetTileStats
        {
            std::uint32_t drawn = 0;
            std::uint32_t ocean = 0;
            std::uint32_t stored = 0;
            std::uint32_t pending = 0;  // demandées, pas encore prêtes
            bool groundLoading = false; // textures du sol en cours de lecture
        };
        PlanetTileStats GetPlanetTileStats() const;
        // Réacteurs de l'image préparée (ThrusterRenderer), leur lumière déjà comptée parmi celles de la scène.
        std::span<const ThrusterDraw> GetThrusters() const { return m_thrusters; }
        // Voir Renderer::SetThrusterPreview.
        void SetThrusterPreview(bool preview) { m_thrusterPreview = preview; }

        void Prepare(const Scene& scene, const Camera& camera, float aspect, Entity hidden, const SkyLighting& sky,
                     const TemporalView& temporal, const std::filesystem::path& assetsDirectory, std::uint32_t frameIndex);
        // Calculs sur le GPU préparés par Prepare (tuiles des planètes), avant toute passe.
        void Compute(RHI::CommandList& commands, std::uint32_t frameIndex);
        // Passe de la carte d'ombres, hors de la passe principale et avant elle ; à chaque image (la carte est
        // effacée puis rendue lisible par Draw).
        void DrawShadows(RHI::CommandList& commands, std::uint32_t frameIndex) const;
        // Passe principale, dans cet ordre (le fond du ciel entre le sol et l'eau). Le sol après les entités : ce
        // qu'elles en cachent (un vaisseau posé dessus) n'est pas éclairé. L'eau avant les autres transparents : vue
        // d'au-dessus de la mer, tout transparent devant elle est plus près (une verrière). Séparés pour que le
        // Renderer mesure chacun au GPU.
        void DrawOpaque(RHI::CommandList& commands, std::uint32_t frameIndex) const;
        void DrawGround(RHI::CommandList& commands, std::uint32_t frameIndex) const;
        void DrawOcean(RHI::CommandList& commands, std::uint32_t frameIndex) const;
        // Nuages des planètes, après l'eau (ils sont devant elle ou cachés par le sol) et avant les autres transparents.
        void DrawClouds(RHI::CommandList& commands, std::uint32_t frameIndex) const;
        void DrawTransparent(RHI::CommandList& commands, std::uint32_t frameIndex) const;

        private:
        // Une entité à dessiner : son modèle et ses matrices, calculées pour l'image.
        struct MeshDraw
        {
            const GpuModel* model;
            Math::LinearColor tint;
            Math::LinearColor emissive; // cube qui émet (MeshRendererComponent::emission), ajouté à celui du matériau
            float metallic = 1.0f;  // multiplie celui du matériau (cube : celui de son MeshRendererComponent)
            float roughness = 1.0f; // idem
            bool materialOverride = false; // metallic et roughness à la place de ceux du matériau (ModelComponent)
            Math::Vector3f center; // de sa boîte, relatif à la caméra
            float radius = 0.0f;   // sphère englobant sa boîte : cascades, champ, ordre
            float distance = 0.0f; // du centre à la caméra : ordre des transparents
            bool visible = true;   // false : ombre seulement (personnage vu de l'intérieur)
            bool inView = true;    // sa sphère touche le champ de la caméra ; sinon ombre seulement
            std::uint32_t firstCluster = 0; // dans m_clusters, si son modèle est découpé en blocs (GpuModel::clusters)
        };
        // Un bloc d'un grand modèle dessiné (GpuModel::clusters), relatif à la caméra.
        struct ClusterDraw
        {
            Math::Vector3f center;
            float radius;
            bool inView;
        };

        // Une tuile de planète à dessiner (toutes sont dans le champ de la caméra : PlanetRenderer::Collect).
        struct TileDraw
        {
            std::uint32_t tile;    // case dans les pages de sommets
            std::uint32_t data;    // dans m_tileData
            Math::Vector3f center; // de sa sphère englobante, relatif à la caméra
            float radius;
            float distance;        // au bord de sa sphère : ordre de près à loin
            bool ocean;            // l'eau la recouvre en partie, et touche le champ de la caméra
        };
        // Appel indirect : `count` commandes de m_tileCommands à partir de `first`, toutes de la même page.
        struct TileBatch
        {
            std::uint32_t page;
            std::uint32_t first;
            std::uint32_t count;
        };
        // Passes des tuiles : le sol, l'eau, puis une par cascade d'ombres.
        static constexpr std::size_t GroundPass = 0;
        static constexpr std::size_t OceanPass = 1;
        static constexpr std::size_t TilePassCount = 2 + ShadowCascadeCount;

        // Doit correspondre à DrawData dans shaders/Mesh.slang : matrices d'une entité, relatives à la caméra.
        struct DrawData
        {
            Math::Matrix4f world;
            // Matrice des normales par colonnes ; normal[1][3] : premier os dans le buffer des os (bits d'un entier)
            std::array<std::array<float, 4>, 3> normal;
            std::array<float, 4> rim; // lumière de contour : couleur x intensité ; finesse (0 : aucune)
            std::array<float, 4> ground; // tuile de planète : xyz, son centre modulo GroundPeriod (textures du sol)
            // world à l'image précédente, relative à sa caméra (mouvement propre pour l'anticrénelage temporel) ; nulle
            // pour les tuiles de planète, immobiles.
            Math::Matrix4f previousWorld;
            // De l'entité, combinés dans le shader à ceux du matériau (DrawParams) : un même appel dessine des entités
            // de teintes différentes.
            std::array<float, 4> tint;     // multiplie la couleur du matériau
            std::array<float, 4> emissive; // ajouté à celui du matériau ; w : 1 si surface remplace métal et rugosité
            std::array<float, 4> surface;  // x : métal, y : rugosité (multiplient ceux du matériau, ou les remplacent)
        };
        // Période (m) des coordonnées des textures du sol : multiple de leurs tailles (GroundScale dans Mesh.slang).
        static constexpr double GroundPeriod = 1024.0;

        // Doit correspondre à LightData dans shaders/Mesh.slang : une lumière de la scène.
        struct LightData
        {
            std::array<float, 4> positionRange; // relative à la caméra ; portée
            std::array<float, 4> color;         // couleur x intensité ; 1 spot, 0 ponctuelle
            std::array<float, 4> direction;     // axe du spot ; cosinus du demi-angle
            std::array<float, 4> spot;          // 1 / (cosinus du début du fondu - cosinus du demi-angle)
        };

        // Lumière retenue pour l'image, et sa distance à la caméra (les plus proches d'abord s'il y en a trop).
        struct PreparedLight
        {
            LightData data;
            float distance;
        };

        // Volume intérieur retenu pour l'image (InteriorComponent), relatif à la caméra.
        struct PreparedInterior
        {
            RelativeTransform transform;
            const InteriorComponent* interior;
            float distance; // de la caméra à la boîte : les plus proches d'abord s'il y en a trop
        };

        // Une primitive de modèle, dessinée en un appel indirect pour toutes les entités du lot (leurs indices dans
        // m_instances, que le shader lit à la place de l'indice d'instance) : commande `command` de m_modelCommands.
        struct ModelBatch
        {
            const RHI::Pipeline* pipeline;
            const GpuModel* model;
            const ModelPrimitive* primitive; // dans GpuModel::primitives ou shadowPrimitives
            std::uint32_t command;
        };
        // Une entité à ranger dans un lot : même pipeline, même modèle et même primitive = même lot.
        struct BatchEntry
        {
            const RHI::Pipeline* pipeline;
            const GpuModel* model;
            const ModelPrimitive* primitive;
            std::uint32_t draw;
            float distance; // au bord de sa sphère
        };
        struct BatchGroup
        {
            std::uint32_t first; // dans m_batchEntries
            std::uint32_t count;
            float distance;      // du plus proche du lot
        };
        // Passes des modèles : opaques, transparents, puis une par cascade d'ombres.
        static constexpr std::size_t OpaqueModelPass = 0;
        static constexpr std::size_t TransparentModelPass = 1;
        static constexpr std::size_t ModelPassCount = 2 + ShadowCascadeCount;

        // Primitive transparente d'une entité, dessinée après les opaques.
        struct TransparentDraw
        {
            std::uint32_t draw;      // dans m_draws
            std::uint32_t primitive; // dans GpuModel::primitives
        };

        // Pipelines d'un type de sommets (statique ou animé).
        struct Pipelines
        {
            // Ombres : profondeur vue du soleil, sans programme de pixel sauf pour le seuil d'alpha.
            std::unique_ptr<RHI::Pipeline> shadow;             // faces tournées vers le soleil seules
            std::unique_ptr<RHI::Pipeline> shadowDoubleSided;  // les deux faces
            std::unique_ptr<RHI::Pipeline> shadowMasked;       // les deux faces, pixels sous le seuil d'alpha jetés
            std::unique_ptr<RHI::Pipeline> opaque;            // faces arrière éliminées, aucun pixel jeté
            std::unique_ptr<RHI::Pipeline> opaqueDoubleSided;  // faces arrière gardées, aucun pixel jeté
            std::unique_ptr<RHI::Pipeline> masked;             // seuil d'alpha, faces arrière éliminées
            std::unique_ptr<RHI::Pipeline> maskedDoubleSided;  // seuil d'alpha, faces arrière gardées
            std::unique_ptr<RHI::Pipeline> transparent;        // mélange alpha, profondeur lue mais pas écrite
            std::unique_ptr<RHI::Pipeline> transparentBack;    // idem, faces arrière seules (double face : avant les faces avant)
        };

        // Relie ce qui change d'un appel à l'autre (pipeline, sommets, groupes), seulement quand il le faut.
        struct BindState
        {
            const RHI::Pipeline* pipeline = nullptr;
            const GpuModel* model = nullptr;
            const RHI::BindGroup* group = nullptr;
            bool frameBound = false;
        };

        explicit MeshRenderer(RHI::Device& device) : m_device(device) {}
        std::expected<void, std::string> CreatePipelines(RHI::Format colorFormat, RHI::Format motionFormat, RHI::Format depthFormat);
        const Pipelines& GetPipelines(const MeshDraw& draw) const;
        bool GrowBuffer(std::unique_ptr<RHI::Buffer>& buffer, std::uint32_t frameIndex, std::uint32_t slot, std::uint64_t size);
        void AddPlanet(const RelativeTransform& transform, const PlanetComponent& planet, const RimLightComponent* rim,
                       const CloudsComponent* clouds, const ViewCone& view);
        // Les nuages d'une planète (ses PlanetGpu::cloud*), dans FrameData.
        void AddClouds(PlanetGpu& data, const RelativeTransform& transform, const PlanetComponent& planet, const CloudsComponent& clouds) const;
        void AddLight(const Camera& camera, float aspect, const RelativeTransform& transform, const LightComponent& light);
        void AddThruster(const Scene& scene, const Camera& camera, float aspect, const RelativeTransform& transform,
                         const ThrusterComponent& thruster);
        void ClearDraws();
        bool BuildTileCommands(std::uint32_t frameIndex);
        void Pose(const GpuModel& model, const AnimatorComponent* animator);
        void Upload(const Camera& camera, float aspect, const SkyLighting& sky, const TemporalView& temporal, std::uint32_t frameIndex);
        static bool IsTransparent(const MeshDraw& draw, const GpuMaterial& material);
        bool InCascade(const Math::Vector3f& center, float radius, std::size_t cascade) const;
        bool CastsShadow(const Math::Vector3f& center, float radius, std::size_t cascade) const;
        bool CastsAnyShadow(const Math::Vector3f& center, float radius) const;
        const ClusterDraw* FindCluster(const MeshDraw& draw, const ModelPrimitive& primitive) const; // nullptr : un bloc
        static const RHI::Pipeline& ShadowPipeline(const Pipelines& pipelines, const GpuMaterial& material);
        static const RHI::Pipeline& OpaquePipeline(const Pipelines& pipelines, const GpuMaterial& material);
        void AddBatches(bool nearestFirst);
        void AddBatch(const BatchEntry& entry, std::uint32_t count);
        bool BuildModelBatches(std::uint32_t frameIndex);
        void BindPipeline(RHI::CommandList& commands, BindState& state, std::uint32_t frameIndex, const RHI::Pipeline& pipeline) const;
        void DrawBatches(RHI::CommandList& commands, BindState& state, std::uint32_t frameIndex, std::size_t pass) const;
        void DrawTiles(RHI::CommandList& commands, BindState& state, std::uint32_t frameIndex, std::size_t pass,
                       const RHI::Pipeline& pipeline) const;

        RHI::Device& m_device;
        std::unique_ptr<ModelCache> m_models;
        std::unique_ptr<PlanetRenderer> m_planets;
        Pipelines m_static;
        // Modèles animés : un second flux de sommets (os et poids), lus avec les matrices des os.
        Pipelines m_skinned;
        // Tuiles de planète : un second flux (forme de la tuile parente), pour glisser d'un niveau de détail à l'autre.
        Pipelines m_planet;
        // Océans : les mêmes tuiles, posées sur la sphère du niveau de la mer (vertexOcean, fragmentOcean).
        Pipelines m_ocean;
        // Nuages des planètes : un triangle plein écran (vertexClouds, fragmentClouds), sans sommets.
        std::unique_ptr<RHI::Pipeline> m_clouds;
        bool m_hasClouds = false; // une planète de l'image a des nuages
        std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now(); // vagues
        // Groupe 1, un par image en vol : os, entités, données de l'image, carte d'ombres.
        std::unique_ptr<RHI::BindGroupLayout> m_frameLayout;
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_jointBuffers;
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_drawBuffers;
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_frameBuffers;
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_lightBuffers;
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_indirectBuffers; // m_tileCommands
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_instanceBuffers; // m_instances
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_modelCommandBuffers; // m_modelCommands
        std::array<std::unique_ptr<RHI::BindGroup>, RHI::FramesInFlight> m_frameGroups;
        // Texture cube du ciel liée au groupe de chaque image (celle de SkyLighting, refaite quand le ciel change).
        std::array<const RHI::Texture*, RHI::FramesInFlight> m_boundEnvironment{};
        // Une seule carte pour les deux images en vol : la passe d'ombres d'une image attend (même file) que la
        // précédente ait fini de la lire.
        std::unique_ptr<RHI::Texture> m_shadowMap;
        std::unique_ptr<RHI::Sampler> m_shadowSampler;
        std::array<ShadowCascade, ShadowCascadeCount> m_cascades{};

        // Préparés sur le CPU avant l'enregistrement des commandes, réutilisés d'une image à l'autre.
        std::vector<RelativeTransform> m_relativeTransforms;
        std::vector<MeshDraw> m_draws;
        std::vector<DrawData> m_drawData; // une par entité de m_draws, envoyées au GPU
        // Par indice d'entité : sa matrice world de la dernière image où elle a été préparée, et le numéro de cette
        // image (m_preparedFrame). Une entité absente de l'image précédente est prise pour immobile.
        std::vector<Math::Matrix4f> m_previousWorld;
        std::vector<std::uint32_t> m_previousFrame;
        std::uint32_t m_preparedFrame = 0;
        std::vector<std::uint32_t> m_opaque;        // dans m_draws, visibles
        std::vector<ClusterDraw> m_clusters;
        std::vector<TransparentDraw> m_transparent; // triés du plus loin au plus proche
        std::vector<Math::Matrix4f> m_jointMatrices;
        std::vector<PreparedLight> m_lights;
        std::vector<LightData> m_lightData; // les retenues, envoyées au GPU
        std::vector<PreparedInterior> m_interiors;
        std::vector<ThrusterDraw> m_thrusters;
        bool m_thrusterPreview = false;
        std::vector<PlanetRenderer::Tile> m_planetTiles;
        std::vector<TileDraw> m_tileDraws;
        std::vector<DrawData> m_tileData; // envoyées au GPU après m_drawData
        std::vector<RHI::DrawIndexedCommand> m_tileCommands;
        std::vector<TileBatch> m_tileBatches;
        std::array<std::span<const TileBatch>, TilePassCount> m_tilePasses; // dans m_tileBatches
        std::vector<BatchEntry> m_batchEntries; // de la passe en cours de rangement
        std::vector<BatchGroup> m_batchGroups;
        std::vector<std::uint32_t> m_instances; // indices dans m_draws, lot après lot
        std::vector<RHI::DrawIndexedCommand> m_modelCommands;
        std::vector<ModelBatch> m_modelBatches;
        std::array<std::span<const ModelBatch>, ModelPassCount> m_modelPasses; // dans m_modelBatches
        std::vector<PlanetGpu> m_planetData; // sol des planètes dessinées, MaxPlanets au plus
        PoseEvaluator m_poseEvaluator;
        GroundTextures m_ground;
        const RHI::BindGroup* m_groundGroup = nullptr; // celui de m_ground pour l'image en préparation
    };
}
