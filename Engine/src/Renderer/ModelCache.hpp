#pragma once

#include "Flux/Animation/Animation.hpp"
#include "Flux/Assets/Model.hpp"
#include "Flux/Math/Box3.hpp"
#include "Flux/Math/Color.hpp"
#include "Renderer/RHI/RHI.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Flux {
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
        // Ses images (couleur de base, émissive, métal-rugosité, normales, occlusion ; blanche si absente) ; nullptr :
        // aucune image, le groupe blanc.
        std::unique_ptr<RHI::BindGroup> group;
    };

    // Modèle 3D envoyé au GPU (le cube du moteur, ou un fichier glTF), et de quoi voir si le fichier a changé.
    struct GpuModel
    {
        std::unique_ptr<RHI::Buffer> vertices;
        std::unique_ptr<RHI::Buffer> indices; // 32 bits
        std::unique_ptr<RHI::Buffer> skin;    // ModelSkinVertex, modèle animé seulement
        ModelSkeleton skeleton;               // gardé sur le CPU : les poses y sont calculées à chaque image
        std::vector<std::unique_ptr<RHI::Texture>> images;
        std::vector<GpuMaterial> materials;
        std::vector<ModelPrimitive> primitives;
        Math::Box3f bounds;
        bool loaded = false;
        std::filesystem::file_time_type writeTime{};
        std::chrono::steady_clock::time_point checked{};

        bool IsAnimated() const { return skin != nullptr; }
    };

    // Modèles des fichiers glTF (et le cube du moteur) sur le GPU : chargés au premier appel, relus si le fichier
    // change, avec leurs matériaux (leurs images, groupe 0 des shaders de maillage).
    class ModelCache {
        public:
        static std::expected<std::unique_ptr<ModelCache>, std::string> Create(RHI::Device& device);

        const RHI::BindGroupLayout& GetMaterialLayout() const { return *m_materialLayout; }
        // Matériau sans image (tous les emplacements sur l'image blanche).
        const RHI::BindGroup& GetWhiteGroup() const { return *m_whiteGroup; }
        const GpuModel& GetCube() const { return m_cube; }

        // Modèle du fichier, chargé au premier appel puis relu s'il change (vérifié une fois par seconde) ; nullptr
        // s'il est illisible (message dans la console).
        const GpuModel* Find(const std::filesystem::path& path);

        private:
        explicit ModelCache(RHI::Device& device) : m_device(device) {}
        std::expected<GpuModel, std::string> Upload(const ModelData& data, std::string_view name);
        void Reload(const std::filesystem::path& path, GpuModel& model);

        RHI::Device& m_device;
        // Dans cet ordre : les groupes (image blanche, matériaux des modèles) sont rendus avant leur layout.
        std::unique_ptr<RHI::Sampler> m_sampler;
        std::unique_ptr<RHI::BindGroupLayout> m_materialLayout;
        std::unique_ptr<RHI::Texture> m_whiteImage; // 1 x 1
        std::unique_ptr<RHI::BindGroup> m_whiteGroup;
        GpuModel m_cube;
        // Par chemin (UTF-8, normalisé) ; les échecs aussi, pour ne pas relire à chaque image.
        std::unordered_map<std::string, GpuModel> m_models;
    };
}
