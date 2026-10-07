#include "Flux/Scene/SceneSerializer.hpp"

#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Scene.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Flux::SceneSerializer {
    namespace {
        using Json = nlohmann::ordered_json; // garde l'ordre des clés : fichiers stables et lisibles

        constexpr std::string_view FormatName = "flux.scene";
        constexpr std::string_view SnapshotFormatName = "flux.entity";

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

        // Clés d'entité gérées ici ; toutes les autres sont des composants.
        bool IsEntityKey(std::string_view key) {
            return key == "id" || key == "name" || key == "parent" || key == "sector";
        }

        // Lecture d'un tableau de N nombres.
        template <typename T, std::size_t N>
        std::array<T, N> ReadArray(const Json& value, std::string_view what) {
            if (!value.is_array() || value.size() != N
                || !std::ranges::all_of(value, [](const Json& item) { return item.is_number(); })) {
                throw std::runtime_error(std::format("« {} » doit être un tableau de {} nombres", what, N));
            }
            std::array<T, N> result{};
            for (std::size_t i = 0; i < N; ++i) {
                result[i] = value[i].get<T>();
            }
            return result;
        }

        Json WriteField(const FieldInfo& field, const void* value) {
            switch (field.type) {
                case FieldType::Bool: return *static_cast<const bool*>(value);
                case FieldType::Int: return *static_cast<const std::int32_t*>(value);
                case FieldType::Float: return Shortest(*static_cast<const float*>(value));
                case FieldType::Double: return *static_cast<const double*>(value);
                case FieldType::Vector3f: return ToJson(*static_cast<const Math::Vector3f*>(value));
                case FieldType::Vector3d: return ToJson(*static_cast<const Math::Vector3d*>(value));
                case FieldType::Quaternion: return ToJson(*static_cast<const Math::Quaternionf*>(value));
                case FieldType::Color: return ToJson(*static_cast<const Math::LinearColor*>(value));
                case FieldType::String: return *static_cast<const std::string*>(value);
                case FieldType::Enum: {
                    const std::int64_t number = field.getEnum(value);
                    for (const EnumOption& option : field.enumOptions) {
                        if (option.value == number) {
                            return option.key;
                        }
                    }
                    return number; // valeur hors de la liste déclarée : gardée en nombre, refusée au chargement
                }
            }
            return nullptr;
        }

        void ReadField(const FieldInfo& field, const Json& json, void* value) {
            const auto expect = [&](bool ok, std::string_view what) {
                if (!ok) {
                    throw std::runtime_error(std::format("« {} » doit être {}", field.key, what));
                }
            };
            // Valeur hors des limites du champ (fichier modifié à la main, ancienne version) : ramenée dedans,
            // comme le ferait l'inspecteur. Une taille nulle ou négative casserait la physique.
            const auto Limit = [&](double number) { return std::clamp(number, field.options.min, field.options.max); };
            switch (field.type) {
                case FieldType::Bool:
                    expect(json.is_boolean(), "true ou false");
                    *static_cast<bool*>(value) = json.get<bool>();
                    break;
                case FieldType::Int:
                    expect(json.is_number_integer() && json.get<std::int64_t>() >= std::numeric_limits<std::int32_t>::min()
                               && json.get<std::int64_t>() <= std::numeric_limits<std::int32_t>::max(),
                           "un entier (32 bits)");
                    *static_cast<std::int32_t*>(value) = json.get<std::int32_t>();
                    break;
                case FieldType::Float:
                    expect(json.is_number(), "un nombre");
                    *static_cast<float*>(value) = static_cast<float>(Limit(json.get<double>()));
                    break;
                case FieldType::Double:
                    expect(json.is_number(), "un nombre");
                    *static_cast<double*>(value) = Limit(json.get<double>());
                    break;
                case FieldType::Vector3f: {
                    const auto v = ReadArray<float, 3>(json, field.key);
                    *static_cast<Math::Vector3f*>(value) = {static_cast<float>(Limit(v[0])), static_cast<float>(Limit(v[1])),
                                                            static_cast<float>(Limit(v[2]))};
                    break;
                }
                case FieldType::Vector3d: {
                    const auto v = ReadArray<double, 3>(json, field.key);
                    *static_cast<Math::Vector3d*>(value) = {Limit(v[0]), Limit(v[1]), Limit(v[2])};
                    break;
                }
                case FieldType::Quaternion: {
                    const auto q = ReadArray<float, 4>(json, field.key);
                    *static_cast<Math::Quaternionf*>(value) = Math::Normalize(Math::Quaternionf{q[0], q[1], q[2], q[3]});
                    break;
                }
                case FieldType::Color: {
                    const auto c = ReadArray<float, 4>(json, field.key);
                    *static_cast<Math::LinearColor*>(value) = {c[0], c[1], c[2], c[3]};
                    break;
                }
                case FieldType::String:
                    expect(json.is_string(), "une chaîne");
                    *static_cast<std::string*>(value) = json.get<std::string>();
                    break;
                case FieldType::Enum: {
                    std::string allowed;
                    for (const EnumOption& option : field.enumOptions) {
                        if (json.is_string() && json.get<std::string>() == option.key) {
                            field.setEnum(value, option.value);
                            return;
                        }
                        allowed += (allowed.empty() ? "« " : ", « ") + option.key + " »";
                    }
                    throw std::runtime_error(std::format("« {} » vaut {}, valeurs possibles : {}", field.key, json.dump(), allowed));
                }
            }
        }

        void WriteEntity(const Scene& scene, const ComponentRegistry& components, Entity entity, Json& entities,
                         bool withDescendants = true) {
            Json json;
            json["id"] = scene.GetId(entity).ToString();
            json["name"] = scene.Get<NameComponent>(entity).name;
            if (const Entity parent = scene.GetParent(entity); parent != NullEntity) {
                json["parent"] = scene.GetId(parent).ToString();
            }
            if (const auto* sector = scene.TryGet<SectorComponent>(entity); sector && sector->sector != Math::Vector3i64{}) {
                json["sector"] = ToJson(sector->sector);
            }

            // Composants enregistrés, dans l'ordre d'enregistrement : le fichier ne change pas d'une sauvegarde à l'autre.
            for (std::size_t i = 0; i < components.GetCount(); ++i) {
                const ComponentInfo& info = components.Get(i);
                if (const void* component = info.get(scene.GetRegistry(), entity)) {
                    Json fields = Json::object();
                    for (const FieldInfo& field : info.fields) {
                        fields[field.key] = WriteField(field, field.access(const_cast<void*>(component)));
                    }
                    json[info.key] = std::move(fields);
                }
            }
            if (const auto* unknown = scene.TryGet<UnknownComponentsComponent>(entity)) {
                for (const auto& [key, text] : unknown->components) {
                    if (!json.contains(key)) { // enregistré entre-temps : la version à jour l'emporte
                        json[key] = Json::parse(text);
                    }
                }
            }
            entities.push_back(std::move(json));

            if (withDescendants) {
                scene.ForEachChild(entity, [&](Entity child) { WriteEntity(scene, components, child, entities, true); });
            }
        }

        struct ComponentData {
            const ComponentInfo* info;
            std::shared_ptr<void> instance;
        };

        // Entité lue et vérifiée, avant toute modification de la scène.
        struct EntityData {
            Uuid id;
            std::string name;
            std::optional<Uuid> parent;
            Math::Vector3i64 sector;
            std::vector<ComponentData> components;
            UnknownComponentsComponent unknown;
        };

        EntityData ReadEntity(const ComponentRegistry& components, const Json& json, std::size_t index) {
            const auto fail = [&](std::string_view message) {
                return std::runtime_error(std::format("entité n°{} : {}", index + 1, message));
            };
            if (!json.is_object()) {
                throw fail("un objet { ... } est attendu");
            }

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

            for (const auto& [key, value] : json.items()) {
                if (IsEntityKey(key)) {
                    continue;
                }
                const ComponentInfo* info = components.Find(key);
                if (!info) {
                    data.unknown.components.emplace_back(key, value.dump());
                    continue;
                }
                if (!value.is_object()) {
                    throw fail(std::format("« {} » doit être un objet {{ ... }}", key));
                }
                // Champs absents : valeurs par défaut du composant, pour pouvoir écrire une scène à la main sans tout préciser.
                ComponentData component{info, info->create()};
                for (const auto& [fieldKey, fieldValue] : value.items()) {
                    const FieldInfo* field = info->FindField(fieldKey);
                    if (!field && std::ranges::contains(info->removedFields, fieldKey)) {
                        continue; // ancienne version : sa valeur est perdue, le champ reprend sa valeur par défaut
                    }
                    if (!field) {
                        throw fail(std::format("« {} » n'a pas de champ « {} »", key, fieldKey));
                    }
                    try {
                        ReadField(*field, fieldValue, field->access(component.instance.get()));
                    } catch (const std::exception& error) {
                        throw fail(std::format("{} : {}", key, error.what()));
                    }
                }
                data.components.push_back(std::move(component));
            }
            return data;
        }
    }

    std::string SaveToString(const Scene& scene, const ComponentRegistry& components) {
        Json entities = Json::array();
        scene.ForEachRoot([&](Entity root) { WriteEntity(scene, components, root, entities); });

        Json document;
        document["format"] = FormatName;
        document["version"] = FormatVersion;
        document["entities"] = std::move(entities);
        return CompactNumberArrays(document.dump(2)) + '\n';
    }

    std::expected<void, std::string> LoadFromString(Scene& scene, const ComponentRegistry& components, std::string_view text) {
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
                entities.push_back(ReadEntity(components, list[i], i));
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
        for (EntityData& data : entities) {
            const Entity entity = scene.FindById(data.id);
            if (data.parent) {
                scene.SetParent(entity, scene.FindById(*data.parent));
            } else if (data.sector != Math::Vector3i64{}) {
                scene.Add<SectorComponent>(entity, data.sector);
            }
            for (const ComponentData& component : data.components) {
                component.info->emplace(scene.GetRegistry(), entity, component.instance.get());
            }
            if (!data.unknown.components.empty()) {
                scene.Add<UnknownComponentsComponent>(entity, std::move(data.unknown));
            }
        }
        return {};
    }

    std::string SaveEntity(const Scene& scene, const ComponentRegistry& components, Entity entity, bool withDescendants) {
        Json entities = Json::array();
        WriteEntity(scene, components, entity, entities, withDescendants);
        Json snapshot;
        snapshot["format"] = SnapshotFormatName;
        if (const Entity next = scene.GetNextSibling(entity); next != NullEntity) {
            snapshot["before"] = scene.GetId(next).ToString();
        }
        snapshot["entities"] = std::move(entities);
        return snapshot.dump();
    }

    std::expected<void, std::string> RestoreEntity(Scene& scene, const ComponentRegistry& components, std::string_view text) {
        std::vector<EntityData> entities;
        std::optional<Uuid> before;
        try {
            const Json snapshot = Json::parse(text);
            if (snapshot.value("format", std::string{}) != SnapshotFormatName) {
                return std::unexpected("ce n'est pas un instantané d'entité");
            }
            if (const auto next = snapshot.find("before"); next != snapshot.end()) {
                before = Uuid::Parse(next->get<std::string>());
            }
            const Json& list = snapshot.at("entities");
            for (std::size_t i = 0; i < list.size(); ++i) {
                entities.push_back(ReadEntity(components, list[i], i));
            }
        } catch (const std::exception& error) {
            return std::unexpected(std::string(error.what()));
        }
        if (entities.empty()) {
            return std::unexpected("instantané vide");
        }
        // Chaque parent doit exister : dans la scène, ou plus haut dans l'instantané (qui liste les parents d'abord).
        for (std::size_t i = 0; i < entities.size(); ++i) {
            const std::optional<Uuid>& parent = entities[i].parent;
            const bool earlier = parent && std::ranges::any_of(entities.begin(), entities.begin() + static_cast<std::ptrdiff_t>(i),
                                                               [&](const EntityData& data) { return data.id == *parent; });
            if (parent && !earlier && scene.FindById(*parent) == NullEntity) {
                return std::unexpected(std::format("le parent de « {} » n'existe plus", entities[i].name));
            }
            if (parent && *parent == entities[i].id) {
                return std::unexpected(std::format("« {} » est son propre parent", entities[i].name));
            }
        }

        for (std::size_t i = 0; i < entities.size(); ++i) {
            EntityData& data = entities[i];
            const Entity parent = data.parent ? scene.FindById(*data.parent) : NullEntity;
            // Seule la première entité a une place à retrouver ; ses descendants suivent dans l'ordre.
            const Entity next = i == 0 && before ? scene.FindById(*before) : NullEntity;
            Entity entity = scene.FindById(data.id);
            if (entity == NullEntity) {
                entity = scene.CreateEntity(data.name, NullEntity, data.id);
                scene.SetParent(entity, parent, next);
            } else {
                scene.Get<NameComponent>(entity).name = data.name;
                // Autre parent, ou autre place parmi ses frères (ordre changé dans la hiérarchie) : remise en place.
                // Un frère de l'instantané introuvable ne déplace rien (pas de retour à la fin par erreur).
                const bool reordered = i == 0 && (!before || next != NullEntity) && scene.GetNextSibling(entity) != next;
                if (scene.GetParent(entity) != parent || reordered) {
                    scene.SetParent(entity, parent, next);
                }
            }
            if (parent == NullEntity && data.sector != Math::Vector3i64{}) {
                scene.Add<SectorComponent>(entity, data.sector);
            } else {
                scene.Remove<SectorComponent>(entity);
            }
            // Composants : ceux de l'instantané remplacent les actuels, les autres (facultatifs) sont retirés.
            for (std::size_t c = 0; c < components.GetCount(); ++c) {
                const ComponentInfo& info = components.Get(c);
                const auto saved = std::ranges::find(data.components, &info, &ComponentData::info);
                if (saved != data.components.end()) {
                    info.emplace(scene.GetRegistry(), entity, saved->instance.get());
                } else if (info.removable && info.get(scene.GetRegistry(), entity)) {
                    info.remove(scene.GetRegistry(), entity);
                }
            }
            if (data.unknown.components.empty()) {
                scene.Remove<UnknownComponentsComponent>(entity);
            } else {
                scene.Add<UnknownComponentsComponent>(entity, std::move(data.unknown));
            }
        }
        return {};
    }

    std::expected<void, std::string> Save(const Scene& scene, const ComponentRegistry& components,
                                          const std::filesystem::path& path) {
        const std::string text = SaveToString(scene, components);
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file.write(text.data(), static_cast<std::streamsize>(text.size()))) {
                return std::unexpected("écriture impossible : " + Platform::ToUtf8(temporary));
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error) {
            return std::unexpected(std::format("remplacement de {} : {}", Platform::ToUtf8(path), error.message()));
        }
        return {};
    }

    std::expected<void, std::string> Load(Scene& scene, const ComponentRegistry& components,
                                          const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return std::unexpected("scène introuvable : " + Platform::ToUtf8(path));
        }
        std::ostringstream text;
        text << file.rdbuf();
        if (auto loaded = LoadFromString(scene, components, text.str()); !loaded) {
            return std::unexpected(Platform::ToUtf8(path.filename()) + " : " + loaded.error());
        }
        return {};
    }
}
