#include "Flux/Renderer/Model.hpp"

#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>

// Implémentation de cgltf compilée ici seulement. Code C tiers : ses avertissements MSVC (fopen, strncpy...) ne
// nous concernent pas.
#define CGLTF_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#pragma warning(disable : 4996)
#endif
#include <cgltf.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace Flux {
    namespace {
        using Math::Vector3f;

        // Matrice 4x4 de cgltf : 16 floats par colonnes.
        using Matrix = std::array<float, 16>;

        constexpr Matrix IdentityMatrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

        Matrix Multiply(const Matrix& a, const Matrix& b) {
            Matrix result{};
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) {
                    float sum = 0.0f;
                    for (int k = 0; k < 4; ++k) {
                        sum += a[k * 4 + r] * b[c * 4 + k];
                    }
                    result[c * 4 + r] = sum;
                }
            }
            return result;
        }

        Matrix Scaled(const Matrix& m, float s) {
            Matrix result = m;
            for (float& value : result) {
                value *= s;
            }
            return result;
        }

        void Accumulate(Matrix& sum, const Matrix& m) {
            for (std::size_t i = 0; i < sum.size(); ++i) {
                sum[i] += m[i];
            }
        }

        Vector3f TransformPoint(const Matrix& m, const Vector3f& p) {
            return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                    m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                    m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
        }

        // Matrice des normales : la transposée de l'inverse de la partie 3x3, c'est-à-dire la comatrice divisée par
        // le déterminant. La normale est renormalisée ensuite : seul le signe du déterminant compte (symétrie).
        // Rangée par lignes.
        std::array<float, 9> NormalMatrix(const Matrix& m) {
            const float a = m[0], b = m[4], c = m[8];
            const float d = m[1], e = m[5], f = m[9];
            const float g = m[2], h = m[6], i = m[10];
            std::array<float, 9> cofactors{
                e * i - f * h, f * g - d * i, d * h - e * g,
                c * h - b * i, a * i - c * g, b * g - a * h,
                b * f - c * e, c * d - a * f, a * e - b * d,
            };
            const float determinant = a * cofactors[0] + b * cofactors[1] + c * cofactors[2];
            if (determinant < 0.0f) {
                for (float& value : cofactors) {
                    value = -value;
                }
            }
            return cofactors;
        }

        Vector3f TransformNormal(const std::array<float, 9>& n, const Vector3f& v) {
            const Vector3f result{n[0] * v.x + n[1] * v.y + n[2] * v.z,
                                  n[3] * v.x + n[4] * v.y + n[5] * v.z,
                                  n[6] * v.x + n[7] * v.y + n[8] * v.z};
            const float length = std::sqrt(Math::Dot(result, result));
            return length > 0.0f ? result / length : Vector3f{0.0f, 1.0f, 0.0f};
        }

        // Fichiers annexes (.bin) lus par nous plutôt que par fopen : chemins Unicode sous Windows.
        cgltf_result ReadFile(const cgltf_memory_options*, const cgltf_file_options*, const char* path,
                              cgltf_size* size, void** data) {
            std::ifstream file(Platform::PathFromUtf8(path), std::ios::binary | std::ios::ate);
            if (!file) {
                return cgltf_result_file_not_found;
            }
            const std::streamoff length = file.tellg();
            if (length < 0) {
                return cgltf_result_io_error;
            }
            file.seekg(0);
            void* bytes = std::malloc(length > 0 ? static_cast<std::size_t>(length) : 1);
            if (!bytes) {
                return cgltf_result_out_of_memory;
            }
            if (!file.read(static_cast<char*>(bytes), length)) {
                std::free(bytes);
                return cgltf_result_io_error;
            }
            *size = static_cast<cgltf_size>(length);
            *data = bytes;
            return cgltf_result_success;
        }

        void ReleaseFile(const cgltf_memory_options*, const cgltf_file_options*, void* data) {
            std::free(data);
        }

        const char* Describe(cgltf_result result) {
            switch (result) {
                case cgltf_result_success: return "succès";
                case cgltf_result_data_too_short: return "fichier tronqué";
                case cgltf_result_unknown_format: return "format inconnu (ni glTF ni GLB)";
                case cgltf_result_invalid_json: return "JSON invalide";
                case cgltf_result_invalid_gltf: return "glTF invalide";
                case cgltf_result_invalid_options: return "options invalides";
                case cgltf_result_file_not_found: return "fichier annexe introuvable (.bin)";
                case cgltf_result_io_error: return "erreur de lecture";
                case cgltf_result_out_of_memory: return "mémoire insuffisante";
                case cgltf_result_legacy_gltf: return "glTF 1.0, non géré (glTF 2.0 seulement)";
                default: return "erreur inconnue";
            }
        }

        std::string Name(const char* name, std::string_view fallback, std::size_t index) {
            if (name && *name) {
                return name;
            }
            return std::string(fallback) + " " + std::to_string(index);
        }

        // Chemin d'un fichier annexe, relatif au modèle ; nullopt pour les données intégrées (data:, .glb).
        std::optional<std::string> ExternalUri(const char* uri) {
            if (!uri || std::string_view(uri).starts_with("data:")) {
                return std::nullopt;
            }
            std::string relative(uri);
            relative.resize(cgltf_decode_uri(relative.data())); // « %20 » -> espace
            return relative;
        }

        struct Loader {
            const cgltf_data& gltf;
            std::filesystem::path directory; // pour les images à côté du fichier
            ModelData model;
            // Image du fichier -> indice dans model.images (-1 : pas encore lue, -2 : illisible).
            std::vector<int> imageSlots;

            int LoadImage(const cgltf_image* image) {
                const std::size_t index = static_cast<std::size_t>(image - gltf.images);
                if (imageSlots[index] != -1) {
                    return imageSlots[index] < 0 ? -1 : imageSlots[index];
                }
                const std::string name = Name(image->name ? image->name : image->uri, "image", index);
                std::expected<ImageData, std::string> pixels = std::unexpected("image sans données");
                if (image->buffer_view) {
                    const std::uint8_t* bytes = cgltf_buffer_view_data(image->buffer_view);
                    if (bytes) {
                        pixels = LoadImageMemory({bytes, image->buffer_view->size});
                    }
                } else if (image->uri) {
                    const std::string_view uri = image->uri;
                    if (uri.starts_with("data:")) {
                        const std::size_t comma = uri.find(";base64,");
                        if (comma == std::string_view::npos) {
                            pixels = std::unexpected("URI data: sans base64");
                        } else {
                            const std::string_view base64 = uri.substr(comma + 8);
                            std::size_t padding = 0;
                            while (padding < 2 && padding < base64.size() && base64[base64.size() - 1 - padding] == '=') {
                                ++padding;
                            }
                            const std::size_t size = base64.size() / 4 * 3 - padding;
                            cgltf_options options{};
                            void* decoded = nullptr;
                            if (cgltf_load_buffer_base64(&options, size, base64.data(), &decoded) == cgltf_result_success) {
                                const std::unique_ptr<void, decltype(&std::free)> owner(decoded, &std::free);
                                pixels = LoadImageMemory({static_cast<const std::uint8_t*>(decoded), size});
                            } else {
                                pixels = std::unexpected("base64 invalide");
                            }
                        }
                    } else {
                        pixels = LoadImageFile(directory / Platform::PathFromUtf8(*ExternalUri(image->uri)));
                    }
                }
                if (!pixels) {
                    model.warnings.push_back("image « " + name + " » ignorée : " + pixels.error());
                    imageSlots[index] = -2;
                    return -1;
                }
                imageSlots[index] = static_cast<int>(model.images.size());
                model.images.push_back(std::move(*pixels));
                return imageSlots[index];
            }

            void LoadMaterials() {
                for (std::size_t i = 0; i < gltf.materials_count; ++i) {
                    const cgltf_material& source = gltf.materials[i];
                    ModelMaterial material;
                    material.name = Name(source.name, "matériau", i);
                    const cgltf_texture* texture = nullptr;
                    if (source.has_pbr_metallic_roughness) {
                        const float* factor = source.pbr_metallic_roughness.base_color_factor;
                        material.baseColor = {factor[0], factor[1], factor[2], factor[3]};
                        texture = source.pbr_metallic_roughness.base_color_texture.texture;
                    } else if (source.has_pbr_specular_glossiness) {
                        // Ancienne extension (exports de 2018-2020) : la couleur diffuse tient lieu de couleur de base.
                        const float* factor = source.pbr_specular_glossiness.diffuse_factor;
                        material.baseColor = {factor[0], factor[1], factor[2], factor[3]};
                        texture = source.pbr_specular_glossiness.diffuse_texture.texture;
                    }
                    if (texture && texture->image) {
                        material.baseColorImage = LoadImage(texture->image);
                    } else if (texture) {
                        model.warnings.push_back("matériau « " + material.name + " » : image dans un format non géré (KTX2, WebP)");
                    }
                    if (source.alpha_mode == cgltf_alpha_mode_mask) {
                        material.alphaCutoff = std::max(source.alpha_cutoff, 1.0e-4f);
                    } else if (source.alpha_mode == cgltf_alpha_mode_blend) {
                        // Pas encore de transparence triée : un seuil à mi-hauteur garde la forme (vitres, cheveux).
                        material.alphaCutoff = 0.5f;
                        model.warnings.push_back("matériau « " + material.name + " » : transparence remplacée par un seuil");
                    }
                    material.doubleSided = source.double_sided;
                    model.materials.push_back(std::move(material));
                }
            }

            // Matériau des primitives qui n'en ont pas : blanc, créé au premier besoin.
            std::uint32_t DefaultMaterial() {
                if (defaultMaterial < 0) {
                    defaultMaterial = static_cast<int>(model.materials.size());
                    ModelMaterial material;
                    material.name = "défaut";
                    model.materials.push_back(std::move(material));
                }
                return static_cast<std::uint32_t>(defaultMaterial);
            }
            int defaultMaterial = -1;

            // Matrices de la pose de repos d'un squelette : pour chaque os, monde * inverse de la pose de liaison.
            std::vector<Matrix> JointMatrices(const cgltf_skin& skin) {
                std::vector<Matrix> joints(skin.joints_count, IdentityMatrix);
                for (std::size_t j = 0; j < skin.joints_count; ++j) {
                    Matrix world;
                    cgltf_node_transform_world(skin.joints[j], world.data());
                    Matrix inverseBind = IdentityMatrix;
                    if (skin.inverse_bind_matrices) {
                        cgltf_accessor_read_float(skin.inverse_bind_matrices, j, inverseBind.data(), 16);
                    }
                    joints[j] = Multiply(world, inverseBind);
                }
                return joints;
            }

            void AddPrimitive(const cgltf_primitive& primitive, const Matrix& world, const cgltf_skin* skin,
                              const std::string& meshName) {
                if (primitive.type != cgltf_primitive_type_triangles && primitive.type != cgltf_primitive_type_triangle_strip
                    && primitive.type != cgltf_primitive_type_triangle_fan) {
                    model.warnings.push_back("maillage « " + meshName + " » : primitive en points ou en lignes ignorée");
                    return;
                }
                const cgltf_accessor* positions = nullptr;
                const cgltf_accessor* normals = nullptr;
                const cgltf_accessor* uvs = nullptr;
                const cgltf_accessor* joints = nullptr;
                const cgltf_accessor* weights = nullptr;
                for (std::size_t a = 0; a < primitive.attributes_count; ++a) {
                    const cgltf_attribute& attribute = primitive.attributes[a];
                    switch (attribute.type) {
                        case cgltf_attribute_type_position: positions = attribute.data; break;
                        case cgltf_attribute_type_normal: normals = attribute.data; break;
                        case cgltf_attribute_type_texcoord:
                            if (attribute.index == 0) {
                                uvs = attribute.data;
                            }
                            break;
                        case cgltf_attribute_type_joints:
                            if (attribute.index == 0) {
                                joints = attribute.data;
                            }
                            break;
                        case cgltf_attribute_type_weights:
                            if (attribute.index == 0) {
                                weights = attribute.data;
                            }
                            break;
                        default: break;
                    }
                }
                if (!positions || positions->count == 0) {
                    return;
                }
                const std::size_t count = positions->count;
                if (model.vertices.size() + count > std::numeric_limits<std::uint32_t>::max()) {
                    model.warnings.push_back("maillage « " + meshName + " » : trop de sommets, ignoré");
                    return;
                }
                const auto base = static_cast<std::uint32_t>(model.vertices.size());

                // Os de la pose de repos (personnage) : chaque sommet suit la moyenne pondérée de ses os, comme
                // à l'affichage dans Blender. Sinon, la transformation du nœud.
                std::vector<Matrix> jointMatrices;
                if (skin && joints && weights) {
                    jointMatrices = JointMatrices(*skin);
                }
                const Matrix* fixed = jointMatrices.empty() ? &world : nullptr;
                const std::array<float, 9> fixedNormals = NormalMatrix(world);

                for (std::size_t v = 0; v < count; ++v) {
                    ModelVertex vertex{};
                    float p[3] = {};
                    cgltf_accessor_read_float(positions, v, p, 3);
                    float n[3] = {0.0f, 0.0f, 0.0f};
                    if (normals) {
                        cgltf_accessor_read_float(normals, v, n, 3);
                    }
                    float t[2] = {};
                    if (uvs) {
                        cgltf_accessor_read_float(uvs, v, t, 2);
                    }
                    Matrix skinned{};
                    const Matrix* transform = fixed;
                    if (!transform) {
                        float jointIndices[4] = {};
                        float jointWeights[4] = {};
                        cgltf_accessor_read_float(joints, v, jointIndices, 4);
                        cgltf_accessor_read_float(weights, v, jointWeights, 4);
                        float total = 0.0f;
                        for (int k = 0; k < 4; ++k) {
                            const auto joint = static_cast<std::size_t>(jointIndices[k]);
                            if (jointWeights[k] > 0.0f && joint < jointMatrices.size()) {
                                Accumulate(skinned, Scaled(jointMatrices[joint], jointWeights[k]));
                                total += jointWeights[k];
                            }
                        }
                        skinned = total > 0.0f ? Scaled(skinned, 1.0f / total) : world;
                        transform = &skinned;
                    }
                    vertex.position = TransformPoint(*transform, {p[0], p[1], p[2]});
                    vertex.normal = TransformNormal(fixed ? fixedNormals : NormalMatrix(*transform), {n[0], n[1], n[2]});
                    vertex.uv = {t[0], t[1]};
                    model.vertices.push_back(vertex);
                }

                // Indices : ceux du fichier, ou 0, 1, 2... sans eux ; bandes et éventails remis en triangles.
                std::vector<std::uint32_t> local;
                const std::size_t indexCount = primitive.indices ? primitive.indices->count : count;
                local.reserve(indexCount);
                for (std::size_t i = 0; i < indexCount; ++i) {
                    const std::size_t index = primitive.indices ? cgltf_accessor_read_index(primitive.indices, i) : i;
                    if (index >= count) {
                        model.warnings.push_back("maillage « " + meshName + " » : indice hors limites, primitive ignorée");
                        model.vertices.resize(base);
                        return;
                    }
                    local.push_back(static_cast<std::uint32_t>(index));
                }
                std::vector<std::uint32_t> triangles;
                if (primitive.type == cgltf_primitive_type_triangles) {
                    triangles = std::move(local);
                    triangles.resize(triangles.size() / 3 * 3);
                } else {
                    for (std::size_t i = 2; i < local.size(); ++i) {
                        if (primitive.type == cgltf_primitive_type_triangle_fan) {
                            triangles.insert(triangles.end(), {local[0], local[i - 1], local[i]});
                        } else if (i % 2 == 0) {
                            triangles.insert(triangles.end(), {local[i - 2], local[i - 1], local[i]});
                        } else {
                            triangles.insert(triangles.end(), {local[i - 1], local[i - 2], local[i]}); // sens gardé
                        }
                    }
                }
                if (triangles.empty()) {
                    model.vertices.resize(base);
                    return;
                }

                // Symétrie (échelle négative) : les triangles changent de sens, on les retourne pour le culling.
                const float determinant = world[0] * (world[5] * world[10] - world[9] * world[6])
                                        - world[4] * (world[1] * world[10] - world[9] * world[2])
                                        + world[8] * (world[1] * world[6] - world[5] * world[2]);
                const bool flip = fixed && determinant < 0.0f;
                for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
                    if (flip) {
                        std::swap(triangles[i + 1], triangles[i + 2]);
                    }
                }

                // Sans normales dans le fichier : normales de face (aspect facetté), sommets dupliqués au besoin.
                if (!normals) {
                    std::vector<ModelVertex> flat;
                    flat.reserve(triangles.size());
                    for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
                        ModelVertex a = model.vertices[base + triangles[i]];
                        ModelVertex b = model.vertices[base + triangles[i + 1]];
                        ModelVertex c = model.vertices[base + triangles[i + 2]];
                        Vector3f normal = Math::Cross(b.position - a.position, c.position - a.position);
                        const float length = std::sqrt(Math::Dot(normal, normal));
                        normal = length > 0.0f ? normal / length : Vector3f{0.0f, 1.0f, 0.0f};
                        a.normal = b.normal = c.normal = normal;
                        flat.insert(flat.end(), {a, b, c});
                    }
                    model.vertices.resize(base);
                    model.vertices.insert(model.vertices.end(), flat.begin(), flat.end());
                    for (std::size_t i = 0; i < triangles.size(); ++i) {
                        triangles[i] = static_cast<std::uint32_t>(i);
                    }
                }

                ModelPrimitive result;
                result.firstIndex = static_cast<std::uint32_t>(model.indices.size());
                result.indexCount = static_cast<std::uint32_t>(triangles.size());
                result.material = primitive.material ? static_cast<std::uint32_t>(primitive.material - gltf.materials)
                                                     : DefaultMaterial();
                for (const std::uint32_t index : triangles) {
                    model.indices.push_back(base + index);
                }
                model.primitives.push_back(result);
            }

            void AddNode(const cgltf_node& node, int depth) {
                if (depth > 256) {
                    return; // hiérarchie cyclique (fichier corrompu) : cgltf_validate l'a normalement refusée
                }
                if (node.mesh) {
                    Matrix world;
                    cgltf_node_transform_world(&node, world.data());
                    const std::string meshName = Name(node.mesh->name, "maillage", static_cast<std::size_t>(node.mesh - gltf.meshes));
                    for (std::size_t p = 0; p < node.mesh->primitives_count; ++p) {
                        AddPrimitive(node.mesh->primitives[p], world, node.skin, meshName);
                    }
                }
                for (std::size_t c = 0; c < node.children_count; ++c) {
                    AddNode(*node.children[c], depth + 1);
                }
            }

            void Load() {
                imageSlots.assign(gltf.images_count, -1);
                LoadMaterials();
                // La scène par défaut, sinon la première, sinon tous les nœuds racines.
                const cgltf_scene* scene = gltf.scene ? gltf.scene : (gltf.scenes_count > 0 ? gltf.scenes : nullptr);
                if (scene) {
                    for (std::size_t n = 0; n < scene->nodes_count; ++n) {
                        AddNode(*scene->nodes[n], 0);
                    }
                } else {
                    for (std::size_t n = 0; n < gltf.nodes_count; ++n) {
                        if (!gltf.nodes[n].parent) {
                            AddNode(gltf.nodes[n], 0);
                        }
                    }
                }
                if (model.materials.empty()) {
                    DefaultMaterial();
                }
                if (!model.vertices.empty()) {
                    Math::Box3f& bounds = model.bounds;
                    bounds.min = bounds.max = model.vertices.front().position;
                    for (const ModelVertex& vertex : model.vertices) {
                        const Vector3f& p = vertex.position;
                        bounds.min = {std::min(bounds.min.x, p.x), std::min(bounds.min.y, p.y), std::min(bounds.min.z, p.z)};
                        bounds.max = {std::max(bounds.max.x, p.x), std::max(bounds.max.y, p.y), std::max(bounds.max.z, p.z)};
                    }
                }
            }
        };
    }

    namespace {
        // Fichier analysé. Pour un .glb, cgltf pointe dans bytes (le bloc binaire) : bytes doit lui survivre,
        // d'où l'ordre des membres (détruits à l'envers).
        struct Parsed
        {
            std::vector<char> bytes;
            std::unique_ptr<cgltf_data, decltype(&cgltf_free)> gltf{nullptr, &cgltf_free};
        };

        // Lit et analyse le fichier (sans charger les .bin).
        std::expected<Parsed, std::string> Parse(const std::filesystem::path& path, const cgltf_options& options) {
            const std::string fileName = Platform::ToUtf8(path.filename());
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                return std::unexpected("impossible d'ouvrir « " + fileName + " »");
            }
            Parsed parsed;
            parsed.bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            if (file.bad()) {
                return std::unexpected("lecture de « " + fileName + " » impossible");
            }
            cgltf_data* data = nullptr;
            const cgltf_result result = cgltf_parse(&options, parsed.bytes.data(), parsed.bytes.size(), &data);
            parsed.gltf.reset(data);
            if (result != cgltf_result_success) {
                return std::unexpected("« " + fileName + " » : " + Describe(result));
            }
            return parsed;
        }

        cgltf_options FileOptions() {
            cgltf_options options{};
            options.file.read = &ReadFile;
            options.file.release = &ReleaseFile;
            return options;
        }
    }

    std::expected<ModelData, std::string> LoadModelFile(const std::filesystem::path& path) {
        const std::string fileName = Platform::ToUtf8(path.filename());
        const cgltf_options options = FileOptions();
        auto parsed = Parse(path, options);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        cgltf_data* gltf = parsed->gltf.get();
        for (std::size_t i = 0; i < gltf->extensions_required_count; ++i) {
            const std::string_view extension = gltf->extensions_required[i];
            // Extensions sans effet sur ce qu'on lit, ou lues directement par cgltf (entiers normalisés).
            if (extension != "KHR_mesh_quantization" && extension != "KHR_texture_transform"
                && extension != "KHR_materials_unlit" && extension != "KHR_materials_emissive_strength") {
                return std::unexpected("« " + fileName + " » : extension " + std::string(extension)
                                       + " non gérée (réexporter sans compression)");
            }
        }
        // Chemin UTF-8 : cgltf y colle le nom des .bin, puis nous le rend dans ReadFile.
        cgltf_result result = cgltf_load_buffers(&options, gltf, Platform::ToUtf8(path).c_str());
        if (result != cgltf_result_success) {
            return std::unexpected("« " + fileName + " » : " + Describe(result));
        }
        result = cgltf_validate(gltf);
        if (result != cgltf_result_success) {
            return std::unexpected("« " + fileName + " » : " + Describe(result));
        }

        Loader loader{*gltf, path.parent_path(), {}, {}};
        loader.Load();
        if (loader.model.indices.empty()) {
            return std::unexpected("« " + fileName + " » : aucun triangle");
        }
        return std::move(loader.model);
    }

    std::expected<std::vector<std::string>, std::string> ListModelFiles(const std::filesystem::path& path) {
        auto parsed = Parse(path, FileOptions());
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        const cgltf_data* gltf = parsed->gltf.get();
        std::vector<std::string> files;
        const auto add = [&files](const char* uri) {
            if (std::optional<std::string> relative = ExternalUri(uri);
                relative && std::ranges::find(files, *relative) == files.end()) {
                files.push_back(std::move(*relative));
            }
        };
        for (std::size_t i = 0; i < gltf->buffers_count; ++i) {
            add(gltf->buffers[i].uri);
        }
        for (std::size_t i = 0; i < gltf->images_count; ++i) {
            add(gltf->images[i].uri);
        }
        return files;
    }
}
