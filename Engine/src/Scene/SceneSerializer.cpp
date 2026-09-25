#include "Flux/Scene/SceneSerializer.hpp"

#include "Flux/Scene/Scene.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Flux::SceneSerializer {
    namespace {
        using Json = nlohmann::ordered_json; // garde l'ordre des clés : fichiers stables et lisibles

        constexpr std::string_view FormatName = "flux.scene";

        // JSON ne stocke que des doubles : un float 0.1f s'écrirait 0.10000000149011612.
        // On passe par l'écriture la plus courte qui redonne exactement ce float (« 0.1 »).
        double Shortest(float value) {
            std::array<char, 32> buffer{};
            const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
            double result = 0.0;
            std::from_chars(buffer.data(), written.ptr, result);
            return result;
        }

        Json ToJson(const Math::Vector3d& v) { return Json::array({v.x, v.y, v.z}); }
        Json ToJson(const Math::Vector3f& v) { return Json::array({Shortest(v.x), Shortest(v.y), Shortest(v.z)}); }
        Json ToJson(const Math::Vector3i64& v) { return Json::array({v.x, v.y, v.z}); }
        Json ToJson(const Math::Quaternionf& q) { return Json::array({Shortest(q.x), Shortest(q.y), Shortest(q.z), Shortest(q.w)}); }
        Json ToJson(const Math::LinearColor& c) { return Json::array({Shortest(c.r), Shortest(c.g), Shortest(c.b), Shortest(c.a)}); }

        // dump(2) met chaque nombre d'un tableau sur sa ligne ; on regroupe les tableaux de nombres sur une seule.
        // Sans risque pour les chaînes : dans un JSON, une chaîne ne contient jamais de vrai retour à la ligne.
        std::string CompactNumberArrays(const std::string& text) {
            std::string result;
            result.reserve(text.size());
            std::size_t i = 0;
            while (i < text.size()) {
                if (text[i] == '[') {
                    const std::size_t close = text.find_first_of("[]{}\"", i + 1);
                    if (close != std::string::npos && text[close] == ']') {
                        const std::string_view inside(text.data() + i + 1, close - i - 1);
                        if (inside.find_first_not_of("0123456789.eE+-, \n") == std::string_view::npos
                            && inside.find_first_not_of(" \n") != std::string_view::npos) {
                            result += '[';
                            bool first = true;
                            std::size_t start = 0;
                            while (start < inside.size()) {
                                std::size_t end = inside.find(',', start);
                                if (end == std::string_view::npos) {
                                    end = inside.size();
                                }
                                const std::string_view item = inside.substr(start, end - start);
                                const std::size_t b = item.find_first_not_of(" \n");
                                const std::size_t e = item.find_last_not_of(" \n");
                                result += first ? "" : ", ";
                                result += item.substr(b, e - b + 1);
                                first = false;
                                start = end + 1;
                            }
                            result += ']';
                            i = close + 1;
                            continue;
                        }
                    }
                }
                result += text[i++];
            }
            return result;
        }

        const char* ToString(MeshType mesh) {
            switch (mesh) {
                case MeshType::Cube: return "cube";
            }
            return "cube";
        }

        std::optional<MeshType> ParseMesh(std::string_view text) {
            if (text == "cube") {
                return MeshType::Cube;
            }
            return std::nullopt;
        }

        // Lecture d'un tableau de N nombres ; lève nlohmann::json::exception si le type ne va pas.
        template <typename T, std::size_t N>
        std::array<T, N> ReadArray(const Json& value, std::string_view what) {
            if (!value.is_array() || value.size() != N) {
                throw std::runtime_error(std::format("« {} » doit être un tableau de {} nombres", what, N));
            }
            std::array<T, N> result{};
            for (std::size_t i = 0; i < N; ++i) {
                result[i] = value[i].get<T>();
            }
            return result;
        }

        void WriteEntity(const Scene& scene, Entity entity, Json& entities) {
            Json json;
            json["id"] = scene.GetId(entity).ToString();
            json["name"] = scene.Get<NameComponent>(entity).name;
            if (const Entity parent = scene.GetParent(entity); parent != NullEntity) {
                json["parent"] = scene.GetId(parent).ToString();
            }
            if (const auto* sector = scene.TryGet<SectorComponent>(entity); sector && sector->sector != Math::Vector3i64{}) {
                json["sector"] = ToJson(sector->sector);
            }

            const auto& transform = scene.Get<TransformComponent>(entity);
            json["transform"] = {
                {"position", ToJson(transform.position)},
                {"rotation", ToJson(transform.rotation)},
                {"scale", ToJson(transform.scale)},
            };

            if (const auto* renderer = scene.TryGet<MeshRendererComponent>(entity)) {
                json["meshRenderer"] = {{"mesh", ToString(renderer->mesh)}, {"color", ToJson(renderer->color)}};
            }
            entities.push_back(std::move(json));

            scene.ForEachChild(entity, [&](Entity child) { WriteEntity(scene, child, entities); });
        }

        // Entité lue et vérifiée, avant toute modification de la scène.
        struct EntityData {
            Uuid id;
            std::string name;
            std::optional<Uuid> parent;
            Math::Vector3i64 sector;
            TransformComponent transform;
            std::optional<MeshRendererComponent> meshRenderer;
        };

        EntityData ReadEntity(const Json& json, std::size_t index) {
            const auto fail = [&](std::string_view message) {
                return std::runtime_error(std::format("entité n°{} : {}", index + 1, message));
            };

            EntityData data;
            const auto id = Uuid::Parse(json.at("id").get<std::string>());
            if (!id) {
                throw fail("« id » doit faire 16 chiffres hexadécimaux, et ne pas valoir 0");
            }
            data.id = *id;
            data.name = json.value("name", std::string{});
            if (const auto parent = json.find("parent"); parent != json.end() && !parent->is_null()) {
                data.parent = Uuid::Parse(parent->get<std::string>());
                if (!data.parent) {
                    throw fail("« parent » n'est pas un identifiant valide");
                }
            }
            if (const auto sector = json.find("sector"); sector != json.end()) {
                const auto s = ReadArray<std::int64_t, 3>(*sector, "sector");
                data.sector = {s[0], s[1], s[2]};
            }
            if (const auto transform = json.find("transform"); transform != json.end()) {
                // Champs absents : valeurs par défaut, pour pouvoir écrire une scène à la main sans tout préciser.
                if (const auto position = transform->find("position"); position != transform->end()) {
                    const auto p = ReadArray<double, 3>(*position, "position");
                    data.transform.position = {p[0], p[1], p[2]};
                }
                if (const auto rotation = transform->find("rotation"); rotation != transform->end()) {
                    const auto r = ReadArray<float, 4>(*rotation, "rotation");
                    data.transform.rotation = Math::Normalize(Math::Quaternionf{r[0], r[1], r[2], r[3]});
                }
                if (const auto scale = transform->find("scale"); scale != transform->end()) {
                    const auto s = ReadArray<float, 3>(*scale, "scale");
                    data.transform.scale = {s[0], s[1], s[2]};
                }
            }
            if (const auto renderer = json.find("meshRenderer"); renderer != json.end()) {
                MeshRendererComponent component;
                const std::string meshName = renderer->value("mesh", std::string{"cube"});
                const auto mesh = ParseMesh(meshName);
                if (!mesh) {
                    throw fail(std::format("maillage inconnu « {} »", meshName));
                }
                component.mesh = *mesh;
                if (const auto color = renderer->find("color"); color != renderer->end()) {
                    const auto c = ReadArray<float, 4>(*color, "color");
                    component.color = {c[0], c[1], c[2], c[3]};
                }
                data.meshRenderer = component;
            }
            return data;
        }
    }

    std::string SaveToString(const Scene& scene) {
        Json entities = Json::array();
        scene.ForEachRoot([&](Entity root) { WriteEntity(scene, root, entities); });

        Json document;
        document["format"] = FormatName;
        document["version"] = FormatVersion;
        document["entities"] = std::move(entities);
        return CompactNumberArrays(document.dump(2)) + '\n';
    }

    std::expected<void, std::string> LoadFromString(Scene& scene, std::string_view text) {
        std::vector<EntityData> entities;
        try {
            const Json document = Json::parse(text);
            if (document.value("format", std::string{}) != FormatName) {
                return std::unexpected(std::format("ce n'est pas une scène Flux (« format » différent de « {} »)", FormatName));
            }
            const int version = document.at("version").get<int>();
            if (version < 1 || version > FormatVersion) {
                return std::unexpected(std::format("version {} non gérée (ce moteur lit jusqu'à la version {})", version, FormatVersion));
            }
            const Json& list = document.at("entities");
            entities.reserve(list.size());
            for (std::size_t i = 0; i < list.size(); ++i) {
                entities.push_back(ReadEntity(list[i], i));
            }
        } catch (const std::exception& error) {
            return std::unexpected(std::string(error.what()));
        }

        // Vérifie les références avant de toucher à la scène.
        std::unordered_map<Uuid, std::size_t> indexById;
        for (std::size_t i = 0; i < entities.size(); ++i) {
            if (!indexById.emplace(entities[i].id, i).second) {
                return std::unexpected(std::format("identifiant {} utilisé deux fois", entities[i].id.ToString()));
            }
        }
        for (const EntityData& data : entities) {
            if (data.parent && !indexById.contains(*data.parent)) {
                return std::unexpected(std::format("« {} » a pour parent {}, absent du fichier", data.name, data.parent->ToString()));
            }
        }
        // Un parent qui serait son propre ancêtre rendrait la hiérarchie infinie.
        for (const EntityData& data : entities) {
            std::size_t steps = 0;
            for (std::optional<Uuid> ancestor = data.parent; ancestor; ancestor = entities[indexById.at(*ancestor)].parent) {
                if (++steps > entities.size()) {
                    return std::unexpected(std::format("boucle dans la hiérarchie autour de « {} »", data.name));
                }
            }
        }

        scene.Clear();
        for (const EntityData& data : entities) {
            scene.CreateEntity(data.name, NullEntity, data.id);
        }
        // Liens posés dans l'ordre du fichier : l'ordre des enfants est conservé.
        for (const EntityData& data : entities) {
            const Entity entity = scene.FindById(data.id);
            if (data.parent) {
                scene.SetParent(entity, scene.FindById(*data.parent));
            } else if (data.sector != Math::Vector3i64{}) {
                scene.Add<SectorComponent>(entity, data.sector);
            }
            scene.Get<TransformComponent>(entity) = data.transform;
            if (data.meshRenderer) {
                scene.Add<MeshRendererComponent>(entity, *data.meshRenderer);
            }
        }
        return {};
    }

    std::expected<void, std::string> Save(const Scene& scene, const std::filesystem::path& path) {
        const std::string text = SaveToString(scene);
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file.write(text.data(), static_cast<std::streamsize>(text.size()))) {
                return std::unexpected("écriture impossible : " + temporary.string());
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error) {
            return std::unexpected(std::format("remplacement de {} : {}", path.string(), error.message()));
        }
        return {};
    }

    std::expected<void, std::string> Load(Scene& scene, const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return std::unexpected("scène introuvable : " + path.string());
        }
        std::ostringstream text;
        text << file.rdbuf();
        if (auto loaded = LoadFromString(scene, text.str()); !loaded) {
            return std::unexpected(path.filename().string() + " : " + loaded.error());
        }
        return {};
    }
}
