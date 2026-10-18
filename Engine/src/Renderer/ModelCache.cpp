#include "Renderer/ModelCache.hpp"

#include "Flux/Assets/Image.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <array>

namespace Flux {
    namespace {
        // Cases des tables partagées par tous les modèles chargés : images (la 0 est l'image blanche) et matériaux (le
        // 0 est le matériau blanc sans image).
        constexpr std::uint32_t MaxImages = 4096;
        constexpr std::uint32_t MaxMaterials = 4096;
        // Pages de géométrie : de quoi tenir la station et quelques vaisseaux dans une seule (32 + 16 Mo) ; les
        // modèles animés à part (os et poids à côté de chaque sommet). Un modèle plus grand a sa propre page.
        constexpr std::uint32_t PageVertices = 1u << 20;
        constexpr std::uint32_t PageIndices = 4u << 20;
        constexpr std::uint32_t AnimatedPageVertices = 1u << 18;
        constexpr std::uint32_t AnimatedPageIndices = 1u << 20;
        // Intervalle entre deux vérifications de la date d'un fichier (relu s'il a changé) : assez court pour voir
        // une retouche aussitôt, assez long pour ne pas interroger le disque à chaque image.
        constexpr std::chrono::milliseconds CheckInterval{1000};

        // Doit correspondre à MaterialData dans shaders/Mesh.slang : une case de la table des matériaux.
        struct MaterialData
        {
            Math::LinearColor color;    // linéaire : couleur de base
            Math::LinearColor emissive; // linéaire : lumière propre (rgb), multipliée par l'image émissive
            float metallic;             // x canal bleu de l'image métal-rugosité
            float roughness;            // x canal vert
            float normalScale;          // 0 : pas de carte de normales
            float occlusionStrength;    // 0 : pas d'occlusion
            float alphaCutoff;          // 0 : pas de seuil
            // Cases de la table des images (0 : blanche) : couleur de base, émissive, métal-rugosité, normales,
            // occlusion.
            std::array<std::uint32_t, 5> images;
            std::array<std::uint32_t, 2> padding;
        };
        static_assert(sizeof(MaterialData) == 80);

        // Indices des images d'un matériau dans MaterialData::images.
        constexpr std::size_t BaseColorImage = 0;
        constexpr std::size_t NormalImage = 3;
        constexpr std::size_t OcclusionImage = 4;

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
            PrepareModel(mesh);
            return mesh;
        }
    }

    GpuModelResources& GpuModelResources::operator=(GpuModelResources&& other) noexcept {
        if (this != &other) {
            Release();
            m_owner = std::exchange(other.m_owner, nullptr);
            page = other.page;
            firstVertex = other.firstVertex;
            vertexCount = std::exchange(other.vertexCount, 0);
            firstIndex = other.firstIndex;
            indexCount = std::exchange(other.indexCount, 0);
            firstImage = other.firstImage;
            imageCount = std::exchange(other.imageCount, 0);
            firstMaterial = other.firstMaterial;
            materialCount = std::exchange(other.materialCount, 0);
            images = std::move(other.images);
        }
        return *this;
    }

    void GpuModelResources::Release() {
        if (m_owner) {
            m_owner->Release(*this);
            m_owner = nullptr;
        }
    }

    ModelCache::~ModelCache() = default;

    void ModelCache::Release(GpuModelResources& resources) {
        GeometryPage& page = m_pages[resources.page];
        page.vertexRanges.Free(resources.firstVertex, resources.vertexCount);
        page.indexRanges.Free(resources.firstIndex, resources.indexCount);
        // Les cases rendues gardent leur image jusqu'à ce qu'une autre la remplace : aucun matériau ne les lit plus.
        m_imageSlots.Free(resources.firstImage, resources.imageCount);
        m_materialSlots.Free(resources.firstMaterial, resources.materialCount);
        resources.vertexCount = resources.indexCount = resources.imageCount = resources.materialCount = 0;
        resources.images.clear();
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
        // Groupe blanc, et ceux des textures du sol (GroundTextures, l'ancien vit le temps d'en faire un nouveau).
        constexpr std::array groundBindings{binding(GroundColorSlot), binding(GroundRoughnessSlot), binding(GroundNormalSlot)};
        auto groundLayout = device.CreateBindGroupLayout({.bindings = groundBindings, .maxGroups = 4});
        if (!groundLayout) {
            return std::unexpected(groundLayout.error());
        }
        cache->m_groundLayout = std::move(*groundLayout);
        constexpr std::array sceneBindings{
            RHI::Binding{ImageTableSlot, RHI::BindingType::TextureTable, RHI::ShaderStage::Fragment, MaxImages},
            RHI::Binding{MaterialTableSlot, RHI::BindingType::StorageBuffer, RHI::ShaderStage::Fragment}};
        auto sceneLayout = device.CreateBindGroupLayout({.bindings = sceneBindings, .maxGroups = 1});
        if (!sceneLayout) {
            return std::unexpected(sceneLayout.error());
        }
        cache->m_sceneLayout = std::move(*sceneLayout);

        constexpr std::array<std::uint8_t, 4> white{255, 255, 255, 255};
        auto image = device.CreateTexture(RHI::Format::RGBA8Srgb, {1, 1}, white);
        if (!image) {
            return std::unexpected(image.error());
        }
        cache->m_whiteImage = std::move(*image);
        cache->m_whiteGroup = device.CreateBindGroup(*cache->m_groundLayout);
        if (!cache->m_whiteGroup) {
            return std::unexpected("allocation du groupe de l'image blanche");
        }
        for (const RHI::Binding& slot : groundBindings) {
            cache->m_whiteGroup->SetTexture(slot.slot, *cache->m_whiteImage, *cache->m_sampler);
        }

        // Tables : l'image blanche et le matériau blanc en case 0, toujours remplies.
        const MaterialData whiteMaterial{{1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, {}, {}};
        std::vector<MaterialData> materials(MaxMaterials, whiteMaterial);
        auto table = device.CreateBuffer({MaxMaterials * sizeof(MaterialData), RHI::BufferUsage::Storage}, materials.data());
        if (!table) {
            return std::unexpected(table.error());
        }
        cache->m_materialTable = std::move(*table);
        cache->m_sceneGroup = device.CreateBindGroup(*cache->m_sceneLayout);
        if (!cache->m_sceneGroup) {
            return std::unexpected("allocation du groupe des tables de matériaux");
        }
        cache->m_sceneGroup->SetTableTexture(ImageTableSlot, 0, *cache->m_whiteImage, *cache->m_sampler);
        cache->m_sceneGroup->SetBuffer(MaterialTableSlot, *cache->m_materialTable);
        cache->m_imageSlots = RangeAllocator(MaxImages);
        cache->m_materialSlots = RangeAllocator(MaxMaterials);
        cache->m_imageSlots.Allocate(1);
        cache->m_materialSlots.Allocate(1);

        auto cube = cache->Upload(BuildCube(), "cube");
        if (!cube) {
            return std::unexpected(cube.error());
        }
        cache->m_cube = std::move(*cube);
        return cache;
    }

    // Place les sommets et les indices du modèle dans une page de géométrie (une nouvelle si aucune n'a la place) et
    // les y envoie.
    bool ModelCache::PlaceGeometry(const ModelData& data, GpuModelResources& resources) {
        const auto vertexCount = static_cast<std::uint32_t>(data.vertices.size());
        const auto indexCount = static_cast<std::uint32_t>(data.indices.size());
        const bool animated = data.IsAnimated();
        std::optional<std::uint32_t> page;
        std::optional<std::uint32_t> firstVertex;
        std::optional<std::uint32_t> firstIndex;
        for (std::uint32_t p = 0; p < m_pages.size() && !page; ++p) {
            GeometryPage& candidate = m_pages[p];
            if (candidate.animated != animated || candidate.vertexRanges.GetLargestFree() < vertexCount
                || candidate.indexRanges.GetLargestFree() < indexCount) {
                continue;
            }
            page = p;
            firstVertex = candidate.vertexRanges.Allocate(vertexCount);
            firstIndex = candidate.indexRanges.Allocate(indexCount);
        }
        if (!page) {
            GeometryPage created;
            created.animated = animated;
            const std::uint32_t vertices = std::max(vertexCount, animated ? AnimatedPageVertices : PageVertices);
            const std::uint32_t indices = std::max(indexCount, animated ? AnimatedPageIndices : PageIndices);
            auto vertexBuffer = m_device.CreateBuffer({std::uint64_t{vertices} * sizeof(ModelVertex), RHI::BufferUsage::Vertex});
            auto indexBuffer = m_device.CreateBuffer({std::uint64_t{indices} * sizeof(std::uint32_t), RHI::BufferUsage::Index});
            if (!vertexBuffer || !indexBuffer) {
                Log::Error("géométrie des modèles : {}", !vertexBuffer ? vertexBuffer.error() : indexBuffer.error());
                return false;
            }
            created.vertices = std::move(*vertexBuffer);
            created.indices = std::move(*indexBuffer);
            if (animated) {
                auto skin = m_device.CreateBuffer({std::uint64_t{vertices} * sizeof(ModelSkinVertex), RHI::BufferUsage::Vertex});
                if (!skin) {
                    Log::Error("géométrie des modèles : {}", skin.error());
                    return false;
                }
                created.skin = std::move(*skin);
            }
            created.vertexRanges = RangeAllocator(vertices);
            created.indexRanges = RangeAllocator(indices);
            firstVertex = created.vertexRanges.Allocate(vertexCount);
            firstIndex = created.indexRanges.Allocate(indexCount);
            page = static_cast<std::uint32_t>(m_pages.size());
            m_pages.push_back(std::move(created));
        }
        resources.page = *page;
        resources.firstVertex = *firstVertex;
        resources.vertexCount = vertexCount;
        resources.firstIndex = *firstIndex;
        resources.indexCount = indexCount;
        GeometryPage& target = m_pages[*page];
        bool sent = m_device.WriteBuffer(*target.vertices, std::uint64_t{*firstVertex} * sizeof(ModelVertex), data.vertices.data(),
                                         data.vertices.size() * sizeof(ModelVertex))
                 && m_device.WriteBuffer(*target.indices, std::uint64_t{*firstIndex} * sizeof(std::uint32_t), data.indices.data(),
                                         data.indices.size() * sizeof(std::uint32_t));
        if (animated) {
            sent = sent && m_device.WriteBuffer(*target.skin, std::uint64_t{*firstVertex} * sizeof(ModelSkinVertex),
                                                data.skinVertices.data(), data.skinVertices.size() * sizeof(ModelSkinVertex));
        }
        return sent;
    }

    // Sommets, indices, images et matériaux envoyés au GPU. Une image refusée par le GPU (ou plus de case libre) laisse
    // son matériau blanc.
    std::expected<GpuModel, std::string> ModelCache::Upload(const ModelData& data, std::string_view name) {
        GpuModel model;
        model.resources = GpuModelResources(*this);
        GpuModelResources& resources = model.resources;
        if (!PlaceGeometry(data, resources)) {
            return std::unexpected("envoi des sommets au GPU");
        }
        model.animated = data.IsAnimated();
        if (model.animated) {
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
        const auto wanted = static_cast<std::uint32_t>(std::ranges::count(colorImages, true) + std::ranges::count(dataImages, true));
        const std::optional<std::uint32_t> firstImage = m_imageSlots.Allocate(wanted);
        if (wanted > 0 && !firstImage) {
            Log::Warning("modèle {} : plus de {} images chargées, ses matériaux restent sans image", name, MaxImages);
        } else {
            resources.firstImage = firstImage.value_or(0);
            resources.imageCount = wanted;
        }
        // Case de la table des images envoyée pour cet indice de ModelData::images ; 0 (blanche) si aucune.
        std::vector<std::uint32_t> colorSlots(data.images.size(), 0);
        std::vector<std::uint32_t> dataSlots(data.images.size(), 0);
        const auto send = [&](std::size_t i, RHI::Format format) -> std::uint32_t {
            if (resources.imageCount == 0) {
                return 0;
            }
            const ImageData& pixels = data.images[i];
            auto image = m_device.CreateTexture(format, {static_cast<uint32_t>(pixels.size.x), static_cast<uint32_t>(pixels.size.y)},
                                                pixels.rgba, true);
            if (!image) {
                Log::Warning("modèle {} : image {} : {}", name, i, image.error());
                return 0;
            }
            const std::uint32_t slot = resources.firstImage + static_cast<std::uint32_t>(resources.images.size());
            m_sceneGroup->SetTableTexture(ImageTableSlot, slot, **image, *m_sampler);
            resources.images.push_back(std::move(*image));
            return slot;
        };
        for (std::size_t i = 0; i < data.images.size(); ++i) {
            if (colorImages[i]) {
                colorSlots[i] = send(i, RHI::Format::RGBA8Srgb);
            }
            if (dataImages[i]) {
                dataSlots[i] = send(i, RHI::Format::RGBA8Unorm);
            }
        }
        const auto slotFor = [](const std::vector<std::uint32_t>& slots, int index) -> std::uint32_t {
            return index >= 0 && static_cast<std::size_t>(index) < slots.size() ? slots[static_cast<std::size_t>(index)] : 0u;
        };

        const auto materialCount = static_cast<std::uint32_t>(data.materials.size());
        const std::optional<std::uint32_t> firstMaterial = m_materialSlots.Allocate(materialCount);
        if (materialCount > 0 && !firstMaterial) {
            Log::Warning("modèle {} : plus de {} matériaux chargés, il reste blanc", name, MaxMaterials);
        } else {
            resources.firstMaterial = firstMaterial.value_or(0);
            resources.materialCount = materialCount;
        }
        std::vector<MaterialData> table;
        for (std::uint32_t m = 0; m < data.materials.size(); ++m) {
            const ModelMaterial& source = data.materials[m];
            GpuMaterial material;
            material.baseColor = source.baseColor;
            material.emissive = source.emissive;
            material.metallic = source.metallic;
            material.roughness = source.roughness;
            material.alphaCutoff = source.alphaCutoff;
            material.doubleSided = source.doubleSided;
            material.blend = source.blend;
            const std::array<std::uint32_t, 5> images{
                slotFor(colorSlots, source.baseColorImage), slotFor(colorSlots, source.emissiveImage),
                slotFor(dataSlots, source.metallicRoughnessImage), slotFor(dataSlots, source.normalImage),
                slotFor(dataSlots, source.occlusionImage)};
            material.normalScale = images[NormalImage] != 0 ? source.normalScale : 0.0f;
            material.occlusionStrength = images[OcclusionImage] != 0 ? source.occlusionStrength : 0.0f;
            if (resources.materialCount > 0) {
                material.index = resources.firstMaterial + m;
                table.push_back({material.baseColor, material.emissive, material.metallic, material.roughness, material.normalScale,
                                 material.occlusionStrength, material.alphaCutoff, images, {}});
            }
            model.materials.push_back(material);
        }
        if (!table.empty() && !m_device.WriteBuffer(*m_materialTable, std::uint64_t{resources.firstMaterial} * sizeof(MaterialData),
                                                    table.data(), table.size() * sizeof(MaterialData))) {
            return std::unexpected("envoi des matériaux au GPU");
        }
        model.primitives = data.primitives;
        model.shadowPrimitives = data.shadowPrimitives;
        for (std::vector<ModelPrimitive>* primitives : {&model.primitives, &model.shadowPrimitives}) {
            for (ModelPrimitive& primitive : *primitives) {
                primitive.firstIndex += resources.firstIndex;
            }
        }
        model.clusters = data.clusters;
        model.bounds = data.bounds;
        model.loaded = true;
        return model;
    }

    void ModelCache::Update() {
        m_loader.TakeFinished(m_ready);
        if (m_ready.empty()) {
            return;
        }
        Install(m_ready.front());
        m_ready.erase(m_ready.begin()); // quelques modèles au plus
    }

    // Remplace le modèle par ce qui a été lu ; en cas d'échec, l'ancienne version (s'il y en a une) reste.
    void ModelCache::Install(ModelLoader::Result& result) {
        const auto found = m_models.find(result.key);
        if (found == m_models.end()) {
            return;
        }
        GpuModel& model = found->second;
        model.loading = false;
        const std::string name = Platform::ToUtf8(Platform::PathFromUtf8(result.key).filename());
        if (!result.data) {
            Log::Error("modèle {}", result.data.error());
            return;
        }
        for (const std::string& warning : result.data->warnings) {
            Log::Warning("modèle {} : {}", name, warning);
        }
        if (model.loaded) {
            // Rechargement (fichier retouché) : les images en vol dessinent peut-être encore l'ancienne version, qui
            // rend ses places aux tables. Rare : attendre le GPU suffit. Un premier chargement n'attend rien : ses
            // cases sont neuves, et ses envois partent avec la prochaine image.
            m_device.WaitIdle();
        }
        auto uploaded = Upload(*result.data, name);
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
        auto [found, created] = m_models.try_emplace(Platform::ToGenericUtf8(path.lexically_normal()));
        return Refresh(found->first, path, found->second, created);
    }

    const GpuModel* ModelCache::Find(const std::filesystem::path& assetsDirectory, std::string_view relativePath) {
        if (assetsDirectory != m_knownDirectory) {
            m_knownDirectory = assetsDirectory;
            m_known.clear();
        }
        if (const auto known = m_known.find(relativePath); known != m_known.end()) {
            return Refresh(*known->second.key, known->second.path, *known->second.model, false);
        }
        std::filesystem::path path = assetsDirectory / Platform::PathFromUtf8(relativePath);
        auto [found, created] = m_models.try_emplace(Platform::ToGenericUtf8(path.lexically_normal()));
        GpuModel& model = found->second;
        const auto known = m_known.emplace(std::string(relativePath), KnownPath{std::move(path), &found->first, &model}).first;
        return Refresh(found->first, known->second.path, model, created);
    }

    // Demande le modèle à sa création, puis le redemande si le fichier a changé (vérifié au plus une fois par
    // CheckInterval, et pas pendant une lecture : le changement est vu à la vérification suivante).
    const GpuModel* ModelCache::Refresh(const std::string& key, const std::filesystem::path& path, GpuModel& model, bool created) {
        const auto now = std::chrono::steady_clock::now();
        if (created || (!model.loading && now - model.checked >= CheckInterval)) {
            model.checked = now;
            std::error_code error;
            const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, error);
            // Fichier absent : essayé une fois (pour le signaler), puis de nouveau quand il apparaît.
            if (created || (!error && writeTime != model.writeTime)) {
                model.writeTime = error ? std::filesystem::file_time_type{} : writeTime;
                model.loading = true;
                m_loader.Request(key, path);
            }
        }
        return model.loaded ? &model : nullptr;
    }
}
