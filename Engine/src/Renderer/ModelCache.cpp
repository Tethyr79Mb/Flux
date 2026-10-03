#include "Renderer/ModelCache.hpp"

#include "Flux/Assets/Image.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <array>

namespace Flux {
    namespace {
        // Matériaux de modèles chargés au plus : un groupe chacun (ses images).
        constexpr std::uint32_t MaxMaterials = 4096;
        // Intervalle entre deux vérifications de la date d'un fichier (relu s'il a changé) : assez court pour voir
        // une retouche aussitôt, assez long pour ne pas interroger le disque à chaque image.
        constexpr std::chrono::milliseconds CheckInterval{1000};

        // Emplacements du groupe 0 (voir shaders/Mesh.slang).
        constexpr std::uint32_t BaseColorSlot = 0;
        constexpr std::uint32_t EmissiveSlot = 1;
        constexpr std::uint32_t MetallicRoughnessSlot = 2;
        constexpr std::uint32_t NormalSlot = 3;
        constexpr std::uint32_t OcclusionSlot = 4;

        // Cube unité centré ; chaque face a ses 4 sommets (normale propre, image entière).
        ModelData BuildCube() {
            struct Face
            {
                Math::Vector3f normal;
                Math::Vector3f tangent;
            };
            constexpr std::array<Face, 6> faces{{
                {{1, 0, 0}, {0, 0, -1}},
                {{-1, 0, 0}, {0, 0, 1}},
                {{0, 1, 0}, {1, 0, 0}},
                {{0, -1, 0}, {1, 0, 0}},
                {{0, 0, 1}, {1, 0, 0}},
                {{0, 0, -1}, {-1, 0, 0}},
            }};
            ModelData mesh;
            for (const Face& face : faces) {
                // bitangent = normal x tangent, donc tangent x bitangent = normal : sens antihoraire vu de l'extérieur.
                const Math::Vector3f bitangent = Math::Cross(face.normal, face.tangent);
                const Math::Vector3f center = face.normal * 0.5f;
                const Math::Vector3f u = face.tangent * 0.5f;
                const Math::Vector3f v = bitangent * 0.5f;
                const auto base = static_cast<uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back({center - u - v, face.normal, {0.0f, 1.0f}});
                mesh.vertices.push_back({center + u - v, face.normal, {1.0f, 1.0f}});
                mesh.vertices.push_back({center + u + v, face.normal, {1.0f, 0.0f}});
                mesh.vertices.push_back({center - u + v, face.normal, {0.0f, 0.0f}});
                for (const uint32_t corner : {0u, 1u, 2u, 0u, 2u, 3u}) {
                    mesh.indices.push_back(base + corner);
                }
            }
            mesh.primitives.push_back({0, static_cast<uint32_t>(mesh.indices.size()), 0});
            mesh.materials.emplace_back();
            return mesh;
        }
    }

    std::expected<std::unique_ptr<ModelCache>, std::string> ModelCache::Create(RHI::Device& device) {
        std::unique_ptr<ModelCache> cache(new ModelCache(device));

        // Répété (les UV des modèles sortent souvent de [0, 1]), filtré entre les mips.
        auto sampler = device.CreateSampler({.filter = RHI::Filter::Linear, .mipFilter = RHI::Filter::Linear,
                                             .address = RHI::AddressMode::Repeat});
        if (!sampler) {
            return std::unexpected(sampler.error());
        }
        cache->m_sampler = std::move(*sampler);

        constexpr auto binding = [](std::uint32_t slot) {
            return RHI::Binding{slot, RHI::BindingType::Texture, RHI::ShaderStage::Fragment};
        };
        constexpr std::array bindings{binding(BaseColorSlot), binding(EmissiveSlot), binding(MetallicRoughnessSlot),
                                      binding(NormalSlot), binding(OcclusionSlot)};
        auto layout = device.CreateBindGroupLayout({.bindings = bindings, .maxGroups = 1 + MaxMaterials});
        if (!layout) {
            return std::unexpected(layout.error());
        }
        cache->m_materialLayout = std::move(*layout);

        constexpr std::array<std::uint8_t, 4> white{255, 255, 255, 255};
        auto image = device.CreateTexture(RHI::Format::RGBA8Srgb, {1, 1}, white);
        if (!image) {
            return std::unexpected(image.error());
        }
        cache->m_whiteImage = std::move(*image);
        cache->m_whiteGroup = device.CreateBindGroup(*cache->m_materialLayout);
        if (!cache->m_whiteGroup) {
            return std::unexpected("allocation du groupe de l'image blanche");
        }
        for (const RHI::Binding& slot : bindings) {
            cache->m_whiteGroup->SetTexture(slot.slot, *cache->m_whiteImage, *cache->m_sampler);
        }

        auto cube = cache->Upload(BuildCube(), "cube");
        if (!cube) {
            return std::unexpected(cube.error());
        }
        cache->m_cube = std::move(*cube);
        return cache;
    }

    // Sommets, indices, images et matériaux envoyés au GPU. Une image refusée par le GPU laisse son matériau blanc.
    std::expected<GpuModel, std::string> ModelCache::Upload(const ModelData& data, std::string_view name) {
        GpuModel model;
        auto vertices = m_device.CreateBuffer({data.vertices.size() * sizeof(ModelVertex), RHI::BufferUsage::Vertex}, data.vertices.data());
        if (!vertices) {
            return std::unexpected(vertices.error());
        }
        model.vertices = std::move(*vertices);
        auto indices = m_device.CreateBuffer({data.indices.size() * sizeof(uint32_t), RHI::BufferUsage::Index}, data.indices.data());
        if (!indices) {
            return std::unexpected(indices.error());
        }
        model.indices = std::move(*indices);
        if (data.IsAnimated()) {
            auto skin = m_device.CreateBuffer({data.skinVertices.size() * sizeof(ModelSkinVertex), RHI::BufferUsage::Vertex},
                                              data.skinVertices.data());
            if (!skin) {
                return std::unexpected(skin.error());
            }
            model.skin = std::move(*skin);
            model.skeleton = data.skeleton;
        }

        // Couleurs (couleur de base, émissive) en sRGB, ramenées en linéaire à la lecture ; données (métal-rugosité,
        // normales, occlusion) lues telles quelles. Une image servant aux deux est envoyée deux fois.
        std::vector<bool> colorImages(data.images.size(), false);
        std::vector<bool> dataImages(data.images.size(), false);
        const auto mark = [](std::vector<bool>& used, int index) {
            if (index >= 0 && static_cast<std::size_t>(index) < used.size()) {
                used[static_cast<std::size_t>(index)] = true;
            }
        };
        for (const ModelMaterial& source : data.materials) {
            mark(colorImages, source.baseColorImage);
            mark(colorImages, source.emissiveImage);
            mark(dataImages, source.metallicRoughnessImage);
            mark(dataImages, source.normalImage);
            mark(dataImages, source.occlusionImage);
        }
        std::vector<const RHI::Texture*> colorTextures(data.images.size(), nullptr);
        std::vector<const RHI::Texture*> dataTextures(data.images.size(), nullptr);
        const auto send = [&](std::size_t i, RHI::Format format) -> const RHI::Texture* {
            const ImageData& pixels = data.images[i];
            auto image = m_device.CreateTexture(format, {static_cast<uint32_t>(pixels.size.x), static_cast<uint32_t>(pixels.size.y)},
                                                pixels.rgba, true);
            if (!image) {
                Log::Warning("modèle {} : image {} : {}", name, i, image.error());
                return nullptr;
            }
            model.images.push_back(std::move(*image));
            return model.images.back().get();
        };
        for (std::size_t i = 0; i < data.images.size(); ++i) {
            if (colorImages[i]) {
                colorTextures[i] = send(i, RHI::Format::RGBA8Srgb);
            }
            if (dataImages[i]) {
                dataTextures[i] = send(i, RHI::Format::RGBA8Unorm);
            }
        }
        // Image envoyée au GPU pour cet indice de ModelData::images ; nullptr si aucune (ou refusée).
        const auto imageFor = [](const std::vector<const RHI::Texture*>& textures, int index) -> const RHI::Texture* {
            return index >= 0 && static_cast<std::size_t>(index) < textures.size() ? textures[static_cast<std::size_t>(index)] : nullptr;
        };
        for (const ModelMaterial& source : data.materials) {
            GpuMaterial material;
            material.baseColor = source.baseColor;
            material.emissive = source.emissive;
            material.metallic = source.metallic;
            material.roughness = source.roughness;
            material.alphaCutoff = source.alphaCutoff;
            material.doubleSided = source.doubleSided;
            material.blend = source.blend;
            const std::array<const RHI::Texture*, 5> images{
                imageFor(colorTextures, source.baseColorImage), imageFor(colorTextures, source.emissiveImage),
                imageFor(dataTextures, source.metallicRoughnessImage), imageFor(dataTextures, source.normalImage),
                imageFor(dataTextures, source.occlusionImage)};
            material.normalScale = images[NormalSlot] ? source.normalScale : 0.0f;
            material.occlusionStrength = images[OcclusionSlot] ? source.occlusionStrength : 0.0f;
            if (std::ranges::any_of(images, [](const RHI::Texture* image) { return image != nullptr; })) {
                material.group = m_device.CreateBindGroup(*m_materialLayout);
                if (material.group) {
                    for (std::uint32_t slot = 0; slot < images.size(); ++slot) {
                        material.group->SetTexture(slot, images[slot] ? *images[slot] : *m_whiteImage, *m_sampler);
                    }
                } else {
                    material.normalScale = 0.0f;
                    material.occlusionStrength = 0.0f;
                    Log::Warning("plus de {} matériaux chargés, « {} » sans image", MaxMaterials, source.name);
                }
            }
            model.materials.push_back(std::move(material));
        }
        model.primitives = data.primitives;
        model.bounds = data.bounds;
        model.loaded = true;
        return model;
    }

    // Lit le fichier et remplace le modèle ; en cas d'échec, l'ancienne version (s'il y en a une) reste.
    void ModelCache::Reload(const std::filesystem::path& path, GpuModel& model) {
        const std::string name = Platform::ToUtf8(path.filename());
        auto data = LoadModelFile(path);
        if (!data) {
            Log::Error("modèle {}", data.error());
            return;
        }
        for (const std::string& warning : data->warnings) {
            Log::Warning("modèle {} : {}", name, warning);
        }
        // Rechargement (fichier retouché) : les images en vol dessinent peut-être encore l'ancienne version.
        // Rare : attendre le GPU suffit, et les envois ci-dessous l'attendent de toute façon.
        m_device.WaitIdle();
        auto uploaded = Upload(*data, name);
        if (!uploaded) {
            Log::Error("modèle {} : {}", name, uploaded.error());
            return;
        }
        const std::filesystem::file_time_type writeTime = model.writeTime;
        const std::chrono::steady_clock::time_point checked = model.checked;
        model = std::move(*uploaded); // l'ancienne version rend ses groupes et ses buffers
        model.writeTime = writeTime;
        model.checked = checked;
    }

    const GpuModel* ModelCache::Find(const std::filesystem::path& path) {
        const auto now = std::chrono::steady_clock::now();
        auto [found, created] = m_models.try_emplace(Platform::ToGenericUtf8(path.lexically_normal()));
        GpuModel& model = found->second;
        if (created || now - model.checked >= CheckInterval) {
            model.checked = now;
            std::error_code error;
            const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, error);
            // Fichier absent : essayé une fois (pour le signaler), puis de nouveau quand il apparaît.
            if (created || (!error && writeTime != model.writeTime)) {
                model.writeTime = error ? std::filesystem::file_time_type{} : writeTime;
                Reload(path, model);
            }
        }
        return model.loaded ? &model : nullptr;
    }
}
