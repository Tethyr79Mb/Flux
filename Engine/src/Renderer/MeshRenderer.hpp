#pragma once

#include "Flux/Animation/Animation.hpp"
#include "Flux/Graphics/ShadowCascades.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Renderer/ModelCache.hpp"
#include "Renderer/PlanetRenderer.hpp"
#include "Renderer/RHI/RHI.hpp"
#include "Renderer/SkyRenderer.hpp"

#include <array>
#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
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
    class MeshRenderer {
        public:
        static std::expected<std::unique_ptr<MeshRenderer>, std::string> Create(RHI::Device& device, RHI::Format colorFormat,
                                                                                 RHI::Format depthFormat);

        ModelCache& GetModels() { return *m_models; }

        // Une matrice par maillage, calculée relativement à la caméra, les os des modèles animés et les cascades
        // d'ombres envoyés au GPU. `hidden` et ses descendants ne sont pas dessinés mais gardent leur ombre. Après
        // Device::BeginFrame : les ressources de l'image frameIndex sont libres.
        void Prepare(const Scene& scene, const Camera& camera, float aspect, Entity hidden, const SkyLighting& sky,
                     const std::filesystem::path& assetsDirectory, std::uint32_t frameIndex);
        // Calculs sur le GPU préparés par Prepare (tuiles des planètes), avant toute passe.
        void Compute(RHI::CommandList& commands, std::uint32_t frameIndex);
        // Passe de la carte d'ombres, hors de la passe principale et avant elle ; à chaque image (la carte est
        // effacée puis rendue lisible par Draw).
        void DrawShadows(RHI::CommandList& commands, std::uint32_t frameIndex) const;
        // Passe principale : les opaques, puis les transparents (le fond du ciel entre les deux).
        void DrawOpaque(RHI::CommandList& commands, std::uint32_t frameIndex) const;
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
            bool ocean = false;    // surface de l'océan d'une tuile de planète (transparente, sans ombre)
        };

        // Doit correspondre à DrawData dans shaders/Mesh.slang : matrices d'une entité, relatives à la caméra.
        struct DrawData
        {
            Math::Matrix4f world;
            // Matrice des normales par colonnes ; normal[1][3] : premier os dans le buffer des os (bits d'un entier)
            std::array<std::array<float, 4>, 3> normal;
            std::array<float, 4> rim; // lumière de contour : couleur x intensité ; finesse (0 : aucune)
        };

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

        // Primitive transparente d'une entité, dessinée après les opaques.
        struct TransparentDraw
        {
            std::uint32_t draw;      // dans m_draws
            std::uint32_t primitive; // dans GpuModel::primitives
        };

        // Pipelines d'un type de sommets (statique ou animé).
        struct Pipelines
        {
            std::unique_ptr<RHI::Pipeline> shadow;            // profondeur vue du soleil, double face
            std::unique_ptr<RHI::Pipeline> opaque;            // faces arrière éliminées
            std::unique_ptr<RHI::Pipeline> opaqueDoubleSided;  // faces arrière gardées
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
        std::expected<void, std::string> CreatePipelines(RHI::Format colorFormat, RHI::Format depthFormat);
        const Pipelines& GetPipelines(const MeshDraw& draw) const;
        bool GrowBuffer(std::unique_ptr<RHI::Buffer>& buffer, std::uint32_t frameIndex, std::uint32_t slot, std::uint64_t size);
        void AddPlanet(const RelativeTransform& transform, const PlanetComponent& planet, const ViewCone& view);
        void AddLight(const Camera& camera, float aspect, const RelativeTransform& transform, const LightComponent& light);
        void Pose(const GpuModel& model, const AnimatorComponent* animator);
        void Upload(const Camera& camera, float aspect, const SkyLighting& sky, std::uint32_t frameIndex);
        static bool IsTransparent(const MeshDraw& draw, const GpuMaterial& material);
        bool InCascade(const MeshDraw& draw, std::size_t cascade) const;
        void DrawPrimitive(RHI::CommandList& commands, BindState& state, std::uint32_t frameIndex, std::uint32_t drawIndex,
                           const ModelPrimitive& primitive, const RHI::Pipeline& pipeline, std::uint32_t cascade = 0) const;

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
        std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now(); // vagues
        // Groupe 1, un par image en vol : os, entités, données de l'image, carte d'ombres.
        std::unique_ptr<RHI::BindGroupLayout> m_frameLayout;
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_jointBuffers;
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_drawBuffers;
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_frameBuffers;
        std::array<std::unique_ptr<RHI::Buffer>, RHI::FramesInFlight> m_lightBuffers;
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
        std::vector<std::uint32_t> m_opaque;        // dans m_draws, visibles, du plus proche au plus loin
        std::vector<TransparentDraw> m_transparent; // triés du plus loin au plus proche
        std::vector<Math::Matrix4f> m_jointMatrices;
        std::vector<PreparedLight> m_lights;
        std::vector<LightData> m_lightData; // les retenues, envoyées au GPU
        std::vector<PreparedInterior> m_interiors;
        std::vector<PlanetRenderer::Tile> m_planetTiles;
        std::vector<PlanetGpu> m_planetData; // sol des planètes dessinées, MaxPlanets au plus
        PoseEvaluator m_poseEvaluator;
    };
}
