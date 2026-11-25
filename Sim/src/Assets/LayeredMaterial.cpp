#include "Flux/Assets/LayeredMaterial.hpp"

#include "Flux/Assets/Image.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <optional>

namespace Flux {
    namespace {
        using Json = nlohmann::json;
        using Result = std::expected<void, std::string>;

        constexpr std::string_view MaterialFormat = "flux.material";
        constexpr std::array<std::string_view, 4> Channels{"r", "g", "b", "a"};

        Result ReadNumber(const Json& object, const char* key, float& value) {
            const auto found = object.find(key);
            if (found == object.end()) {
                return {};
            }
            if (!found->is_number()) {
                return std::unexpected(std::format("« {} » doit être un nombre", key));
            }
            value = found->get<float>();
            return {};
        }

        Result ReadText(const Json& object, const char* key, std::string& value) {
            const auto found = object.find(key);
            if (found == object.end()) {
                return {};
            }
            if (!found->is_string()) {
                return std::unexpected(std::format("« {} » doit être un texte", key));
            }
            value = found->get<std::string>();
            return {};
        }

        Result ReadLayer(const Json& object, LayeredMaterialLayer& layer) {
            if (!object.is_object()) {
                return std::unexpected("chaque couche doit être un objet { ... }");
            }
            ModelMaterialLayer& s = layer.settings;
            for (const auto& [key, text] : {std::pair{"name", &layer.name}, std::pair{"colorImage", &layer.colorImage},
                                            std::pair{"normalImage", &layer.normalImage},
                                            std::pair{"roughnessImage", &layer.roughnessImage},
                                            std::pair{"metallicImage", &layer.metallicImage},
                                            std::pair{"heightImage", &layer.heightImage}}) {
                if (auto read = ReadText(object, key, *text); !read) {
                    return read;
                }
            }
            if (const auto found = object.find("normalDirectX"); found != object.end()) {
                if (!found->is_boolean()) {
                    return std::unexpected("« normalDirectX » doit être true ou false");
                }
                layer.normalDirectX = found->get<bool>();
            }
            // Sans image de métal, une couche est de la matière non métallique tant qu'on ne dit pas le contraire.
            s.metallic = layer.metallicImage.empty() ? 0.0f : 1.0f;
            for (const auto& [key, value] : {std::pair{"metallic", &s.metallic}, std::pair{"roughness", &s.roughness},
                                             std::pair{"tiling", &s.tiling}, std::pair{"normalScale", &s.normalScale},
                                             std::pair{"opacity", &s.opacity}, std::pair{"sharpness", &s.sharpness},
                                             std::pair{"heightInfluence", &s.heightInfluence}}) {
                if (auto read = ReadNumber(object, key, *value); !read) {
                    return read;
                }
            }
            if (const auto tint = object.find("tint"); tint != object.end()) {
                if (!tint->is_array() || tint->size() != 3 || !std::ranges::all_of(*tint, &Json::is_number)) {
                    return std::unexpected("« tint » doit être [rouge, vert, bleu] (linéaire, 1 : inchangé)");
                }
                s.tint = {(*tint)[0].get<float>(), (*tint)[1].get<float>(), (*tint)[2].get<float>(), 1.0f};
            }
            std::string mask = "none";
            if (auto read = ReadText(object, "mask", mask); !read) {
                return read;
            }
            const auto channel = std::ranges::find(Channels, mask);
            if (channel == Channels.end() && mask != "none") {
                return std::unexpected(std::format("couche « {} » : « mask » vaut r, g, b, a ou none", layer.name));
            }
            s.maskChannel = channel == Channels.end() ? -1 : static_cast<int>(channel - Channels.begin());
            // Bornes : une répétition nulle ou un bord infiniment net n'ont pas de sens (division, NaN dans le shader).
            s.tiling = std::max(s.tiling, 1.0e-3f);
            s.sharpness = std::max(s.sharpness, 1.0f);
            s.opacity = std::clamp(s.opacity, 0.0f, 1.0f);
            s.metallic = std::clamp(s.metallic, 0.0f, 1.0f);
            s.roughness = std::clamp(s.roughness, 0.0f, 1.0f);
            return {};
        }

        // Images d'un fichier, lues une fois même si plusieurs couches les partagent.
        class ImageReader {
            public:
            ImageReader(const std::filesystem::path& assets, std::vector<std::filesystem::path>& dependencies)
                : m_assets(assets), m_dependencies(dependencies) {}

            const ImageData* Read(const std::string& relative) {
                if (relative.empty()) {
                    return nullptr;
                }
                auto [found, created] = m_images.try_emplace(relative);
                if (created) {
                    const std::filesystem::path path = m_assets / Platform::PathFromUtf8(relative);
                    m_dependencies.push_back(path);
                    auto image = LoadImageFile(path);
                    if (image) {
                        found->second = std::move(*image);
                    } else {
                        Log::Warning("matériau multicouche : image {} : {}", relative, image.error());
                    }
                }
                return found->second ? &*found->second : nullptr;
            }

            private:
            const std::filesystem::path& m_assets;
            std::vector<std::filesystem::path>& m_dependencies;
            std::map<std::string, std::optional<ImageData>> m_images;
        };

        // Canal `channel` du pixel (x, y) d'une image de taille `size`, lu au plus proche dans `image` (tailles
        // différentes : rare, les bibliothèques donnent toutes les images d'une matière à la même taille).
        std::uint8_t Fetch(const ImageData* image, Math::Vector2i size, int x, int y, int channel, std::uint8_t missing) {
            if (!image) {
                return missing;
            }
            const int sx = image->size.x == size.x ? x : x * image->size.x / size.x;
            const int sy = image->size.y == size.y ? y : y * image->size.y / size.y;
            return image->rgba[(static_cast<std::size_t>(sy) * static_cast<std::size_t>(image->size.x) + static_cast<std::size_t>(sx)) * 4
                               + static_cast<std::size_t>(channel)];
        }

        // Assemble les images d'une couche en deux (couleur et hauteur ; normale, rugosité, métal) : deux lectures par
        // pixel au GPU au lieu de cinq.
        void PackLayer(const LayeredMaterialLayer& source, ImageReader& reader, ModelData& data, ModelMaterialLayer& layer) {
            const ImageData* color = reader.Read(source.colorImage);
            const ImageData* height = reader.Read(source.heightImage);
            const ImageData* normal = reader.Read(source.normalImage);
            const ImageData* roughness = reader.Read(source.roughnessImage);
            const ImageData* metallic = reader.Read(source.metallicImage);
            if (!height) {
                layer.heightInfluence = 0.0f; // pas de relief connu : le bord ne suit que le masque
            }
            const auto build = [&](const ImageData* first, const ImageData* second, auto texel) -> int {
                const ImageData* reference = first ? first : second;
                if (!reference) {
                    return -1;
                }
                ImageData packed;
                packed.size = reference->size;
                packed.rgba.resize(static_cast<std::size_t>(packed.size.x) * static_cast<std::size_t>(packed.size.y) * 4);
                std::size_t i = 0;
                for (int y = 0; y < packed.size.y; ++y) {
                    for (int x = 0; x < packed.size.x; ++x, i += 4) {
                        texel(packed.size, x, y, &packed.rgba[i]);
                    }
                }
                data.images.push_back(std::move(packed));
                return static_cast<int>(data.images.size()) - 1;
            };
            layer.colorImage = build(color, height, [&](Math::Vector2i size, int x, int y, std::uint8_t* out) {
                for (int c = 0; c < 3; ++c) {
                    out[c] = Fetch(color, size, x, y, c, 255);
                }
                out[3] = Fetch(height, size, x, y, 0, 128);
            });
            const ImageData* dataReference = normal ? normal : roughness ? roughness : metallic;
            layer.dataImage = build(dataReference, nullptr, [&](Math::Vector2i size, int x, int y, std::uint8_t* out) {
                out[0] = Fetch(normal, size, x, y, 0, 128);
                const std::uint8_t green = Fetch(normal, size, x, y, 1, 128);
                out[1] = source.normalDirectX && normal ? static_cast<std::uint8_t>(255 - green) : green;
                out[2] = Fetch(roughness, size, x, y, 0, 255);
                out[3] = Fetch(metallic, size, x, y, 0, 255);
            });
            if (!normal) {
                layer.normalScale = 0.0f; // image de données sans relief (rugosité ou métal seuls) : normale plate
            }
        }
    }

    std::expected<LayeredMaterial, std::string> ParseLayeredMaterial(std::string_view text) {
        const Json document = Json::parse(text.begin(), text.end(), nullptr, false);
        if (document.is_discarded() || !document.is_object()) {
            return std::unexpected("JSON illisible (attendu : un objet { ... })");
        }
        LayeredMaterial material;
        if (auto read = ReadText(document, "mask", material.maskImage); !read) {
            return std::unexpected(read.error());
        }
        const auto layers = document.find("layers");
        if (layers == document.end() || !layers->is_array()) {
            return std::unexpected("« layers » doit être une liste de couches [ { ... } ]");
        }
        if (layers->size() > MaxMaterialLayers) {
            return std::unexpected(std::format("{} couches, au plus {}", layers->size(), MaxMaterialLayers));
        }
        for (const Json& object : *layers) {
            LayeredMaterialLayer layer;
            if (auto read = ReadLayer(object, layer); !read) {
                return std::unexpected(read.error());
            }
            material.layers.push_back(std::move(layer));
        }
        return material;
    }

    std::string SerializeLayeredMaterial(const LayeredMaterial& material) {
        Json document{{"format", std::string(MaterialFormat)}, {"version", 1}};
        if (!material.maskImage.empty()) {
            document["mask"] = material.maskImage;
        }
        Json layers = Json::array();
        for (const LayeredMaterialLayer& layer : material.layers) {
            const ModelMaterialLayer& s = layer.settings;
            Json object{{"name", layer.name}};
            for (const auto& [key, text] : {std::pair{"colorImage", &layer.colorImage}, std::pair{"normalImage", &layer.normalImage},
                                            std::pair{"roughnessImage", &layer.roughnessImage},
                                            std::pair{"metallicImage", &layer.metallicImage},
                                            std::pair{"heightImage", &layer.heightImage}}) {
                if (!text->empty()) {
                    object[key] = *text;
                }
            }
            if (layer.normalDirectX) {
                object["normalDirectX"] = true;
            }
            object["mask"] = std::string(s.maskChannel >= 0 && s.maskChannel < 4 ? Channels[static_cast<std::size_t>(s.maskChannel)] : "none");
            object["tint"] = {s.tint.r, s.tint.g, s.tint.b};
            object["metallic"] = s.metallic;
            object["roughness"] = s.roughness;
            object["tiling"] = s.tiling;
            object["normalScale"] = s.normalScale;
            object["opacity"] = s.opacity;
            object["sharpness"] = s.sharpness;
            object["heightInfluence"] = s.heightInfluence;
            layers.push_back(std::move(object));
        }
        document["layers"] = std::move(layers);
        return document.dump(2) + "\n";
    }

    std::filesystem::path GetMaterialListPath(const std::filesystem::path& model) {
        std::filesystem::path path = model;
        path.replace_extension(".materiaux.json");
        return path;
    }

    std::string MaterialPreviewKey(const std::filesystem::path& path) {
        return Platform::ToGenericUtf8(path.lexically_normal());
    }

    std::string SerializeMaterialList(const MaterialList& list) {
        Json materials = Json::object();
        for (const auto& [name, file] : list) {
            materials[name] = file;
        }
        const Json document{{"format", "flux.materials"}, {"version", 1}, {"materials", std::move(materials)}};
        return document.dump(2) + "\n";
    }

    std::expected<MaterialList, std::string> ParseMaterialList(std::string_view text) {
        const Json document = Json::parse(text.begin(), text.end(), nullptr, false);
        if (document.is_discarded() || !document.is_object()) {
            return std::unexpected("JSON illisible (attendu : un objet { ... })");
        }
        const auto materials = document.find("materials");
        if (materials == document.end() || !materials->is_object()) {
            return std::unexpected("« materials » doit être { \"matériau du glTF\": \"Materiaux/fichier.fluxmat\", ... }");
        }
        MaterialList list;
        for (const auto& [name, file] : materials->items()) {
            if (!file.is_string()) {
                return std::unexpected(std::format("matériau « {} » : le fichier doit être un texte", name));
            }
            list.emplace_back(name, file.get<std::string>());
        }
        return list;
    }

    void ApplyLayeredMaterials(ModelData& data, const std::filesystem::path& modelPath, const std::filesystem::path& assetsDirectory,
                               std::vector<std::filesystem::path>& dependencies, const MaterialPreviews* previews) {
        // Texte de l'aperçu de l'éditeur s'il y en a un, sinon le fichier ; false : ni l'un ni l'autre.
        std::string text;
        const auto read = [&](const std::filesystem::path& path) {
            if (previews) {
                if (const auto found = previews->find(MaterialPreviewKey(path)); found != previews->end()) {
                    text = found->second;
                    return true;
                }
            }
            std::vector<std::uint8_t> bytes;
            std::error_code missing;
            if (!std::filesystem::exists(path, missing) || !Platform::ReadFileBytes(path, bytes)) {
                return false;
            }
            text.assign(bytes.begin(), bytes.end());
            return true;
        };
        const std::filesystem::path listPath = GetMaterialListPath(modelPath);
        dependencies.push_back(listPath); // suivie même absente : le moteur voit apparaître le fichier
        // Sans dossier des assets (pas de projet), les chemins de la liste ne mènent nulle part.
        if (assetsDirectory.empty() || !read(listPath)) {
            return;
        }
        const std::string listName = Platform::ToGenericUtf8(listPath.filename());
        const auto list = ParseMaterialList(text);
        if (!list) {
            Log::Warning("{} : {}", listName, list.error());
            return;
        }
        ImageReader reader(assetsDirectory, dependencies);
        for (const auto& [name, file] : *list) {
            const auto target = std::ranges::find(data.materials, name, &ModelMaterial::name);
            if (target == data.materials.end()) {
                Log::Warning("{} : le modèle n'a pas de matériau « {} »", listName, name);
                continue;
            }
            const std::filesystem::path path = assetsDirectory / Platform::PathFromUtf8(file);
            dependencies.push_back(path);
            if (!read(path)) {
                Log::Warning("{} : matériau « {} » : fichier {} illisible", listName, name, file);
                continue;
            }
            auto material = ParseLayeredMaterial(text);
            if (!material) {
                Log::Warning("{} : {}", file, material.error());
                continue;
            }
            target->layers.clear();
            for (const LayeredMaterialLayer& source : material->layers) {
                ModelMaterialLayer layer = source.settings;
                PackLayer(source, reader, data, layer);
                target->layers.push_back(layer);
            }
            target->layerMaskImage = -1;
            if (const ImageData* mask = reader.Read(material->maskImage)) {
                data.images.push_back(*mask);
                target->layerMaskImage = static_cast<int>(data.images.size()) - 1;
            } else if (std::ranges::any_of(target->layers, [](const ModelMaterialLayer& l) { return l.maskChannel >= 0; })) {
                Log::Warning("{} : couches masquées sans masque lisible : posées partout", file);
            }
        }
    }
}
