#pragma once

#include "Flux/Animation/Animation.hpp"
#include "Flux/Assets/Model.hpp"
#include "Flux/Math/Box3.hpp"
#include "Flux/Math/Color.hpp"
#include "Renderer/ModelLoader.hpp"
#include "Renderer/RHI/RHI.hpp"
#include "Renderer/RangeAllocator.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Flux {
    // Ce que le GPU sait d'un matériau est dans la table des matériaux (MaterialData dans shaders/Mesh.slang, case
    // `index`) ; le CPU garde de quoi choisir le pipeline et la passe.
    struct GpuMaterial
    {
        Math::LinearColor baseColor{1.0f, 1.0f, 1.0f, 1.0f};
        Math::LinearColor emissive{0.0f, 0.0f, 0.0f, 1.0f}; // multiplie l'image émissive
        float metallic = 1.0f;          // multiplie le canal bleu de l'image métal-rugosité
        float roughness = 1.0f;         // multiplie son canal vert
        float normalScale = 0.0f;       // 0 : pas de carte de normales
        float occlusionStrength = 0.0f; // 0 : pas d'image d'occlusion
        float alphaCutoff = 0.0f;
        bool doubleSided = false;
        bool blend = false; // transparent : dessiné dans la passe triée
        std::uint32_t index = 0; // dans la table des matériaux (0 : blanc, sans image)
    };

    class ModelCache;

    // Places d'un modèle dans les tables partagées de ModelCache (sommets et indices d'une page de géométrie, cases
    // de la table des images, de la table des matériaux), et ses images. Rendues à sa destruction : le GPU ne doit
    // plus dessiner le modèle (ModelCache::Install attend le GPU avant de remplacer un modèle).
    class GpuModelResources {
        public:
        GpuModelResources() = default;
        explicit GpuModelResources(ModelCache& owner) : m_owner(&owner) {}
        ~GpuModelResources() { Release(); }
        GpuModelResources(const GpuModelResources&) = delete;
        GpuModelResources& operator=(const GpuModelResources&) = delete;
        GpuModelResources(GpuModelResources&& other) noexcept { *this = std::move(other); }
        GpuModelResources& operator=(GpuModelResources&& other) noexcept;

        std::uint32_t page = 0;        // page de géométrie (ModelCache::GetPage)
        std::uint32_t firstVertex = 0; // dans les sommets de la page (et les os, page de modèles animés)
        std::uint32_t vertexCount = 0;
        std::uint32_t firstIndex = 0;  // dans les indices de la page
        std::uint32_t indexCount = 0;
        std::uint32_t firstImage = 0;  // cases de la table des images
        std::uint32_t imageCount = 0;
        std::uint32_t firstMaterial = 0; // cases de la table des matériaux
        std::uint32_t materialCount = 0;
        std::vector<std::unique_ptr<RHI::Texture>> images;

        private:
        void Release();
        ModelCache* m_owner = nullptr;
    };

    // Modèle 3D envoyé au GPU (le cube du moteur, ou un fichier glTF), et de quoi voir si le fichier a changé. Ses
    // sommets et indices sont dans une page de géométrie partagée avec d'autres modèles : un seul appel indirect
    // dessine des modèles différents (MeshRenderer).
    struct GpuModel
    {
        GpuModelResources resources;
        ModelSkeleton skeleton; // gardé sur le CPU : les poses y sont calculées à chaque image
        std::vector<GpuMaterial> materials;
        // firstIndex : dans les indices de la page ; indices relatifs au premier sommet du modèle (resources.firstVertex).
        std::vector<ModelPrimitive> primitives;
        std::vector<ModelPrimitive> shadowPrimitives; // ModelData::shadowPrimitives
        std::vector<Math::Box3f> clusters;            // ModelData::clusters ; vide : un seul bloc
        Math::Box3f bounds;
        bool animated = false; // page de modèles animés : os et poids (ModelSkinVertex) à côté des sommets
        bool loaded = false;
        bool loading = false; // fichier en lecture (ModelLoader) ; l'ancienne version, s'il y en a une, reste dessinée
        std::filesystem::file_time_type writeTime{};
        std::chrono::steady_clock::time_point checked{};

        bool IsAnimated() const { return animated; }
    };

    // Page de géométrie : sommets, indices (32 bits) et, pour les modèles animés, os et poids de plusieurs modèles,
    // chacun à sa place (GpuModelResources).
    struct GeometryPage
    {
        std::unique_ptr<RHI::Buffer> vertices; // ModelVertex
        std::unique_ptr<RHI::Buffer> indices;
        std::unique_ptr<RHI::Buffer> skin;     // ModelSkinVertex, une par sommet ; page de modèles animés seulement
        RangeAllocator vertexRanges;
        RangeAllocator indexRanges;
        bool animated = false;
    };

    // Modèles des fichiers glTF (et le cube du moteur) sur le GPU : demandés au premier appel, lus sur un autre fil
    // (ModelLoader) puis envoyés au GPU par Update, relus de même si le fichier change. Partagés par tous les modèles : les pages de géométrie, la table des images et celle des matériaux
    // (groupe 2 des shaders de maillage, GetSceneGroup), lus par indice dans les shaders.
    class ModelCache {
        public:
        static std::expected<std::unique_ptr<ModelCache>, std::string> Create(RHI::Device& device);
        ~ModelCache();

        // Emplacements du groupe 0, où les tuiles de planète lisent les textures du sol (GroundTextures).
        static constexpr std::uint32_t GroundColorSlot = 5;
        static constexpr std::uint32_t GroundNormalSlot = 6;
        // Emplacements du groupe 2 (voir shaders/Mesh.slang).
        static constexpr std::uint32_t ImageTableSlot = 0;
        static constexpr std::uint32_t MaterialTableSlot = 1;

        // Groupe 0 : textures du sol des planètes (GroundTextures) ; les modèles n'en lisent rien.
        const RHI::BindGroupLayout& GetGroundLayout() const { return *m_groundLayout; }
        // Groupe 0 sans textures du sol (images blanches).
        const RHI::BindGroup& GetWhiteGroup() const { return *m_whiteGroup; }
        // Groupe 2 : tables des images et des matériaux de tous les modèles.
        const RHI::BindGroupLayout& GetSceneLayout() const { return *m_sceneLayout; }
        const RHI::BindGroup& GetSceneGroup() const { return *m_sceneGroup; }
        const GpuModel& GetCube() const { return m_cube; }
        const GeometryPage& GetPage(std::uint32_t page) const { return m_pages[page]; }

        // Envoie au GPU les modèles lus depuis l'image précédente (un par image : chacun copie ses sommets et ses images
        // vers la mémoire vidéo). Une fois par image, avant Find.
        void Update();
        // Modèles demandés et pas encore envoyés au GPU.
        std::size_t GetLoadingCount() const { return m_loader.GetPending() + m_ready.size(); }

        // Modèle du fichier, demandé au premier appel puis relu s'il change (vérifié une fois par seconde) ; nullptr
        // tant qu'il n'est pas arrivé, ou s'il est illisible (message dans la console).
        const GpuModel* Find(const std::filesystem::path& path);
        // Même chose par chemin relatif au dossier des assets, sans allocation une fois le modèle connu : appelée pour
        // chaque objet à chaque image.
        const GpuModel* Find(const std::filesystem::path& assetsDirectory, std::string_view relativePath);

        private:
        friend class GpuModelResources;
        explicit ModelCache(RHI::Device& device) : m_device(device) {}
        std::expected<GpuModel, std::string> Upload(const ModelData& data, std::string_view name);
        bool PlaceGeometry(const ModelData& data, GpuModelResources& resources);
        void Release(GpuModelResources& resources);
        void Install(ModelLoader::Result& result);
        const GpuModel* Refresh(const std::string& key, const std::filesystem::path& path, GpuModel& model, bool created);

        struct StringHash
        {
            using is_transparent = void;
            std::size_t operator()(std::string_view text) const { return std::hash<std::string_view>{}(text); }
        };
        struct KnownPath
        {
            std::filesystem::path path; // complet, pour relire le fichier
            const std::string* key;     // et le modèle, dans m_models (les éléments d'un unordered_map ne bougent pas)
            GpuModel* model;
        };

        RHI::Device& m_device;
        // Dans cet ordre : les groupes sont rendus avant leur layout, et les modèles (qui rendent leurs places aux
        // tables) avant les tables.
        std::unique_ptr<RHI::Sampler> m_sampler;
        std::unique_ptr<RHI::BindGroupLayout> m_groundLayout;
        std::unique_ptr<RHI::BindGroupLayout> m_sceneLayout;
        std::unique_ptr<RHI::Texture> m_whiteImage; // 1 x 1, case 0 de la table des images
        std::unique_ptr<RHI::BindGroup> m_whiteGroup;
        std::unique_ptr<RHI::Buffer> m_materialTable; // MaterialData, une case par matériau
        std::unique_ptr<RHI::BindGroup> m_sceneGroup;
        RangeAllocator m_imageSlots;
        RangeAllocator m_materialSlots;
        std::vector<GeometryPage> m_pages;
        GpuModel m_cube;
        // Par chemin (UTF-8, normalisé) ; les échecs aussi, pour ne pas relire à chaque image.
        std::unordered_map<std::string, GpuModel> m_models;
        // Chemins relatifs déjà vus, pour le dossier m_knownDirectory.
        std::filesystem::path m_knownDirectory;
        std::unordered_map<std::string, KnownPath, StringHash, std::equal_to<>> m_known;
        std::vector<ModelLoader::Result> m_ready; // lus, en attente d'envoi (Update)
        ModelLoader m_loader; // en dernier : ses fils s'arrêtent avant que le reste disparaisse
    };
}
