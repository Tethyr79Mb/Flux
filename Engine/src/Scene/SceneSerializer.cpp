#include "Flux/Scene/SceneSerializer.hpp"

#include "Flux/Platform/Filesystem.hpp"
#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Scene.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <functional>
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
        constexpr std::string_view PrefabFormatName = "flux.prefab";

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
            return key == "id" || key == "name" || key == "parent" || key == "sector" || key == "prefab" || key == "overrides"
                || key == "prefabSource" || key == "prefabInstance";
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

        // Identifiant d'une pièce d'un exemplaire de prefab : celui de l'entité dans le prefab, mélangé à celui de
        // l'exemplaire. Le même à chaque chargement, différent d'un exemplaire à l'autre. Un ou exclusif : on retrouve
        // `source` depuis l'identifiant de la pièce (SavePrefab écrit ainsi un prefab dont l'entité d'origine est
        // l'exemplaire, sans changer ses identifiants).
        Uuid MemberId(Uuid instance, Uuid source) {
            std::uint64_t mix = instance.value; // splitmix64
            mix = (mix ^ (mix >> 30)) * 0xbf58476d1ce4e5b9ULL;
            mix = (mix ^ (mix >> 27)) * 0x94d049bb133111ebULL;
            mix ^= mix >> 31;
            const std::uint64_t value = source.value ^ mix;
            return {value != 0 ? value : 1};
        }

        // Pièce de l'exemplaire dont `root` est la racine (la racine comprise).
        bool IsMember(const Scene& scene, Entity entity, Entity root) {
            const auto* link = scene.TryGet<PrefabLinkComponent>(entity);
            return link && link->instance == root;
        }

        Json ComponentToJson(const ComponentInfo& info, const void* component) {
            Json fields = Json::object();
            for (const FieldInfo& field : info.fields) {
                fields[field.key] = WriteField(field, field.access(const_cast<void*>(component)));
            }
            return fields;
        }

        // Composants de l'entité, sous leur clé : enregistrés dans l'ordre d'enregistrement (le fichier ne change
        // pas d'une sauvegarde à l'autre), puis les inconnus gardés tels quels.
        void WriteComponents(const Scene& scene, const ComponentRegistry& components, Entity entity, Json& json) {
            for (std::size_t i = 0; i < components.GetCount(); ++i) {
                const ComponentInfo& info = components.Get(i);
                if (const void* component = info.get(scene.GetRegistry(), entity)) {
                    json[info.key] = ComponentToJson(info, component);
                }
            }
            if (const auto* unknown = scene.TryGet<UnknownComponentsComponent>(entity)) {
                for (const auto& [key, text] : unknown->components) {
                    if (!json.contains(key)) { // enregistré entre-temps : la version à jour l'emporte
                        json[key] = Json::parse(text);
                    }
                }
            }
        }

        // Scene : fichier de scène (exemplaires de prefabs réduits à leurs différences). Snapshot : instantané, tout
        // écrit, liens aux prefabs compris. Prefab : tout écrit, sans lien (SavePrefab refuse les exemplaires).
        enum class WriteMode : std::uint8_t { Scene, Snapshot, Prefab };

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
            // Exemplaire de prefab : chemin du prefab (racine seulement), entité du prefab et racine de l'exemplaire
            // (pièces, racine comprise).
            std::string prefab;
            Uuid source;
            Uuid instance;
        };

        EntityData ReadEntity(const ComponentRegistry& components, const Json& json, std::size_t index);

        // Lecture d'un prefab (texte d'un .prefab.json) : ses entités, la racine d'abord. Lève une erreur.
        std::vector<EntityData> ReadPrefab(const ComponentRegistry& components, std::string_view text) {
            const Json document = Json::parse(text);
            if (document.value("format", std::string{}) != PrefabFormatName) {
                throw std::runtime_error(std::format("ce n'est pas un prefab Flux (« format » différent de « {} »)", PrefabFormatName));
            }
            const int version = document.at("version").get<int>();
            if (version < 1 || version > FormatVersion) {
                throw std::runtime_error(std::format("version {} non gérée (ce moteur lit jusqu'à la version {})", version, FormatVersion));
            }
            const Json& list = document.at("entities");
            std::vector<EntityData> entities;
            entities.reserve(list.size());
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (list[i].is_object() && (list[i].contains("prefab") || list[i].contains("prefabSource"))) {
                    throw std::runtime_error("un prefab dans un prefab n'est pas encore géré");
                }
                entities.push_back(ReadEntity(components, list[i], i));
            }
            // Une racine, puis ses descendants : chacun après son parent.
            if (entities.empty() || entities[0].parent) {
                throw std::runtime_error("la première entité doit être la racine du prefab");
            }
            for (std::size_t i = 1; i < entities.size(); ++i) {
                const std::optional<Uuid>& parent = entities[i].parent;
                if (!parent || std::ranges::none_of(entities.begin(), entities.begin() + static_cast<std::ptrdiff_t>(i),
                                                    [&](const EntityData& data) { return data.id == *parent; })) {
                    throw std::runtime_error(std::format("« {} » doit suivre son parent, dans le prefab", entities[i].name));
                }
            }
            return entities;
        }

        // Ce que la scène garde d'un prefab pour ne pas le relire à chaque exemplaire : son texte, ou l'erreur.
        class PrefabTexts {
            public:
            explicit PrefabTexts(const PrefabReader& reader) : m_reader(reader) {}

            const std::expected<std::string, std::string>& Get(const std::string& path) {
                auto found = m_texts.find(path);
                if (found == m_texts.end()) {
                    found = m_texts.emplace(path, m_reader ? m_reader(path)
                                                           : std::unexpected(std::string("aucun dossier d'assets où le lire"))).first;
                }
                return found->second;
            }

            private:
            const PrefabReader& m_reader;
            std::unordered_map<std::string, std::expected<std::string, std::string>> m_texts;
        };

        // Entités du prefab au format du fichier (champs absents du fichier remis à leur valeur par défaut), par
        // identifiant dans le prefab : la base dont un exemplaire n'écrit que les différences. Vide si le prefab est
        // illisible : tout est alors écrit.
        struct Baseline {
            std::string name;
            Json components;
        };
        std::unordered_map<Uuid, Baseline> ReadBaseline(const ComponentRegistry& components, PrefabTexts& prefabs,
                                                        const std::string& path) {
            std::unordered_map<Uuid, Baseline> baseline;
            const auto& text = prefabs.Get(path);
            if (!text) {
                return baseline;
            }
            try {
                for (const EntityData& data : ReadPrefab(components, *text)) {
                    Json json = Json::object();
                    for (const ComponentData& component : data.components) {
                        json[component.info->key] = ComponentToJson(*component.info, component.instance.get());
                    }
                    for (const auto& [key, value] : data.unknown.components) {
                        json[key] = Json::parse(value);
                    }
                    baseline.emplace(data.id, Baseline{data.name, std::move(json)});
                }
            } catch (const std::exception&) {
                baseline.clear(); // prefab abîmé : tout écrire, rien ne se perd
            }
            return baseline;
        }

        // Ce qui diffère du prefab sur une pièce : nom, champs changés, composants ajoutés (en entier), retirés.
        // Un composant inconnu (non enregistré) est écrit en entier s'il diffère : on ne sait pas lire ses champs à part.
        Json Differences(const ComponentRegistry& components, const Json& current, const std::string& name, const Baseline* base,
                         bool root) {
            Json differences = Json::object();
            if (!root && (!base || base->name != name)) {
                differences["name"] = name;
            }
            for (const auto& [key, value] : current.items()) {
                if (!base || !base->components.contains(key)) {
                    differences[key] = value;
                    continue;
                }
                const auto original = base->components.find(key);
                if (components.Find(key) && value.is_object() && original->is_object()) {
                    Json fields = Json::object();
                    for (const auto& [field, fieldValue] : value.items()) {
                        if (const auto before = original->find(field); before == original->end() || *before != fieldValue) {
                            fields[field] = fieldValue;
                        }
                    }
                    if (!fields.empty()) {
                        differences[key] = std::move(fields);
                    }
                } else if (value != *original) {
                    differences[key] = value;
                }
            }
            if (base) {
                Json removed = Json::array();
                for (const auto& [key, value] : base->components.items()) {
                    if (!current.contains(key)) {
                        removed.push_back(key);
                    }
                }
                if (!removed.empty()) {
                    differences["removed"] = std::move(removed);
                }
            }
            return differences;
        }

        void WriteEntity(const Scene& scene, const ComponentRegistry& components, Entity entity, Json& entities,
                         bool withDescendants, WriteMode mode, PrefabTexts* prefabs);

        // Exemplaire de prefab dans un fichier de scène : sa racine, le chemin du prefab, les différences de chaque
        // pièce (par identifiant dans le prefab), puis les entités ajoutées sous ses pièces.
        void WriteInstance(const Scene& scene, const ComponentRegistry& components, Entity root, Json& entities,
                           PrefabTexts& prefabs) {
            const std::string& path = scene.Get<PrefabInstanceComponent>(root).prefab;
            const std::unordered_map<Uuid, Baseline> baseline = ReadBaseline(components, prefabs, path);
            Json json;
            json["id"] = scene.GetId(root).ToString();
            json["name"] = scene.Get<NameComponent>(root).name;
            if (const Entity parent = scene.GetParent(root); parent != NullEntity) {
                json["parent"] = scene.GetId(parent).ToString();
            }
            if (const auto* sector = scene.TryGet<SectorComponent>(root); sector && sector->sector != Math::Vector3i64{}) {
                json["sector"] = ToJson(sector->sector);
            }
            json["prefab"] = path;
            Json overrides = Json::object();
            std::vector<Entity> added; // entités ajoutées sous les pièces, écrites après l'exemplaire
            std::function<void(Entity)> visit = [&](Entity member) {
                const Uuid source = scene.Get<PrefabLinkComponent>(member).source;
                const auto base = baseline.find(source);
                // Pièce retirée du prefab depuis : elle disparaît avec lui (sauf prefab illisible : tout est gardé).
                if (base != baseline.end() || baseline.empty()) {
                    Json current = Json::object();
                    WriteComponents(scene, components, member, current);
                    Json differences = Differences(components, current, scene.Get<NameComponent>(member).name,
                                                   base != baseline.end() ? &base->second : nullptr, member == root);
                    if (!differences.empty()) {
                        overrides[source.ToString()] = std::move(differences);
                    }
                }
                scene.ForEachChild(member, [&](Entity child) {
                    if (IsMember(scene, child, root)) {
                        visit(child);
                    } else {
                        added.push_back(child);
                    }
                });
            };
            visit(root);
            if (!overrides.empty()) {
                json["overrides"] = std::move(overrides);
            }
            entities.push_back(std::move(json));
            for (const Entity child : added) {
                WriteEntity(scene, components, child, entities, true, WriteMode::Scene, &prefabs);
            }
        }

        void WriteEntity(const Scene& scene, const ComponentRegistry& components, Entity entity, Json& entities,
                         bool withDescendants, WriteMode mode, PrefabTexts* prefabs) {
            if (mode == WriteMode::Scene && scene.TryGet<PrefabInstanceComponent>(entity) && IsMember(scene, entity, entity)) {
                WriteInstance(scene, components, entity, entities, *prefabs);
                return;
            }
            Json json;
            json["id"] = scene.GetId(entity).ToString();
            json["name"] = scene.Get<NameComponent>(entity).name;
            if (const Entity parent = scene.GetParent(entity); parent != NullEntity) {
                json["parent"] = scene.GetId(parent).ToString();
            }
            if (const auto* sector = scene.TryGet<SectorComponent>(entity); sector && sector->sector != Math::Vector3i64{}) {
                json["sector"] = ToJson(sector->sector);
            }
            if (mode == WriteMode::Snapshot) {
                if (const auto* instance = scene.TryGet<PrefabInstanceComponent>(entity)) {
                    json["prefab"] = instance->prefab;
                }
                if (const auto* link = scene.TryGet<PrefabLinkComponent>(entity); link && scene.IsValid(link->instance)) {
                    json["prefabSource"] = link->source.ToString();
                    json["prefabInstance"] = scene.GetId(link->instance).ToString();
                }
            }
            WriteComponents(scene, components, entity, json);
            entities.push_back(std::move(json));

            if (withDescendants) {
                scene.ForEachChild(entity, [&](Entity child) { WriteEntity(scene, components, child, entities, true, mode, prefabs); });
            }
        }

        // Applique à une pièce ce qu'un exemplaire change de son prefab (voir Differences).
        void ApplyOverrides(const ComponentRegistry& components, EntityData& data, const Json& overrides, bool root) {
            if (!overrides.is_object()) {
                throw std::runtime_error(std::format("« overrides » de « {} » : un objet {{ ... }} est attendu", data.name));
            }
            for (const auto& [key, value] : overrides.items()) {
                if (key == "name") {
                    if (!root) {
                        data.name = value.get<std::string>();
                    }
                    continue;
                }
                if (key == "removed") {
                    for (const Json& removed : value) {
                        const std::string component = removed.get<std::string>();
                        std::erase_if(data.components, [&](const ComponentData& c) { return c.info->key == component; });
                        std::erase_if(data.unknown.components, [&](const auto& c) { return c.first == component; });
                    }
                    continue;
                }
                const ComponentInfo* info = components.Find(key);
                if (!info) {
                    std::erase_if(data.unknown.components, [&](const auto& c) { return c.first == key; });
                    data.unknown.components.emplace_back(key, value.dump());
                    continue;
                }
                if (!value.is_object()) {
                    throw std::runtime_error(std::format("« {} » de « {} » doit être un objet {{ ... }}", key, data.name));
                }
                auto component = std::ranges::find(data.components, info, &ComponentData::info);
                if (component == data.components.end()) {
                    data.components.push_back({info, info->create()});
                    component = data.components.end() - 1;
                }
                for (const auto& [fieldKey, fieldValue] : value.items()) {
                    const FieldInfo* field = info->FindField(fieldKey);
                    if (!field && std::ranges::contains(info->removedFields, fieldKey)) {
                        continue;
                    }
                    if (!field) {
                        throw std::runtime_error(std::format("« {} » n'a pas de champ « {} »", key, fieldKey));
                    }
                    try {
                        ReadField(*field, fieldValue, field->access(component->instance.get()));
                    } catch (const std::exception& error) {
                        throw std::runtime_error(std::format("« {} », {} : {}", data.name, key, error.what()));
                    }
                }
            }
        }

        // Entités d'un exemplaire de prefab : celles du prefab, avec les identifiants de l'exemplaire et ses
        // différences. `root` : la racine telle que lue dans la scène (identifiant, nom, parent, secteur).
        void ExpandInstance(const ComponentRegistry& components, const std::string& text, EntityData root,
                            const Json* overrides, std::vector<EntityData>& out) {
            std::vector<EntityData> entities = ReadPrefab(components, text);
            const Uuid rootSource = entities[0].id;
            for (std::size_t i = 0; i < entities.size(); ++i) {
                EntityData& data = entities[i];
                const Uuid source = data.id;
                if (i == 0) {
                    data.id = root.id;
                    data.name = root.name;
                    data.parent = root.parent;
                    data.sector = root.sector;
                    data.prefab = root.prefab;
                } else {
                    data.id = MemberId(root.id, source);
                    data.parent = *data.parent == rootSource ? root.id : MemberId(root.id, *data.parent);
                }
                data.source = source;
                data.instance = root.id;
                if (overrides) {
                    if (const auto found = overrides->find(source.ToString()); found != overrides->end()) {
                        ApplyOverrides(components, data, *found, i == 0);
                    }
                }
                out.push_back(std::move(data));
            }
        }

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
            data.prefab = json.value("prefab", std::string{});
            if (const auto source = json.find("prefabSource"); source != json.end()) {
                const auto sourceId = Uuid::Parse(source->get<std::string>());
                const auto instanceId = Uuid::Parse(json.value("prefabInstance", std::string{}));
                if (!sourceId || !instanceId) {
                    throw fail("« prefabSource » et « prefabInstance » doivent être des identifiants valides");
                }
                data.source = *sourceId;
                data.instance = *instanceId;
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

        // Liste d'entités d'un fichier (scène ou prefab), exemplaires de prefabs dépliés.
        std::vector<EntityData> ReadEntities(const ComponentRegistry& components, const Json& list, PrefabTexts& prefabs) {
            std::vector<EntityData> entities;
            entities.reserve(list.size());
            for (std::size_t i = 0; i < list.size(); ++i) {
                EntityData data = ReadEntity(components, list[i], i);
                if (data.prefab.empty()) {
                    entities.push_back(std::move(data));
                    continue;
                }
                const std::string path = data.prefab;
                const auto& text = prefabs.Get(path);
                if (!text) {
                    throw std::runtime_error(std::format("« {} » : prefab {} illisible : {}", data.name, path, text.error()));
                }
                const auto overrides = list[i].find("overrides");
                try {
                    ExpandInstance(components, *text, std::move(data), overrides != list[i].end() ? &*overrides : nullptr, entities);
                } catch (const std::exception& error) {
                    throw std::runtime_error(std::format("prefab {} : {}", path, error.what()));
                }
            }
            return entities;
        }

        // Crée les entités lues (parents d'abord ou plus haut dans la liste, ou déjà dans la scène), avec leurs
        // composants et leurs liens aux prefabs. Liens posés dans l'ordre de la liste : l'ordre des enfants est conservé.
        void CreateEntities(Scene& scene, std::vector<EntityData>& entities) {
            for (const EntityData& data : entities) {
                scene.CreateEntity(data.name, NullEntity, data.id);
            }
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
                if (!data.prefab.empty()) {
                    scene.Add<PrefabInstanceComponent>(entity, data.prefab);
                }
                if (data.source) {
                    scene.Add<PrefabLinkComponent>(entity, scene.FindById(data.instance), data.source);
                }
            }
        }
    }

    PrefabReader ReadPrefabsFrom(std::filesystem::path assetsDirectory) {
        // Textes gardés tant que le fichier ne change pas (date et taille) : l'inspecteur compare chaque image une
        // pièce à son prefab sans relire le disque.
        struct Cached
        {
            std::filesystem::file_time_type time;
            std::uintmax_t size = 0;
            std::string text;
        };
        auto cache = std::make_shared<std::unordered_map<std::string, Cached>>();
        return [directory = std::move(assetsDirectory), cache](std::string_view path) -> std::expected<std::string, std::string> {
            const std::filesystem::path file = directory / Platform::PathFromUtf8(path);
            std::error_code error;
            const auto time = std::filesystem::last_write_time(file, error);
            const auto size = error ? 0 : std::filesystem::file_size(file, error);
            if (error) {
                cache->erase(std::string(path));
                return std::unexpected(std::format("fichier introuvable : {}", path));
            }
            if (const auto found = cache->find(std::string(path)); found != cache->end() && found->second.time == time
                                                                    && found->second.size == size) {
                return found->second.text;
            }
            std::ifstream stream(file, std::ios::binary);
            if (!stream) {
                return std::unexpected(std::format("fichier illisible : {}", path));
            }
            std::ostringstream text;
            text << stream.rdbuf();
            (*cache)[std::string(path)] = {time, size, text.str()};
            return text.str();
        };
    }

    std::string SaveToString(const Scene& scene, const ComponentRegistry& components, const PrefabReader& prefabs) {
        PrefabTexts texts(prefabs);
        Json entities = Json::array();
        scene.ForEachRoot([&](Entity root) { WriteEntity(scene, components, root, entities, true, WriteMode::Scene, &texts); });

        Json document;
        document["format"] = FormatName;
        document["version"] = FormatVersion;
        document["entities"] = std::move(entities);
        return CompactNumberArrays(document.dump(2)) + '\n';
    }

    std::expected<void, std::string> LoadFromString(Scene& scene, const ComponentRegistry& components, std::string_view text,
                                                    const PrefabReader& prefabs) {
        PrefabTexts texts(prefabs);
        std::vector<EntityData> entities;
        try {
            const Json document = Json::parse(text);
            const std::string format = document.value("format", std::string{});
            if (format != FormatName && format != PrefabFormatName) {
                return std::unexpected(std::format("ce n'est pas une scène Flux (« format » différent de « {} »)", FormatName));
            }
            const int version = document.at("version").get<int>();
            if (version < 1 || version > FormatVersion) {
                return std::unexpected(std::format("version {} non gérée (ce moteur lit jusqu'à la version {})", version, FormatVersion));
            }
            entities = ReadEntities(components, document.at("entities"), texts);
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
            if (data.source && !indexById.contains(data.instance)) {
                return std::unexpected(std::format("« {} » est la pièce d'un prefab absent du fichier", data.name));
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
        CreateEntities(scene, entities);
        return {};
    }

    std::string SaveEntity(const Scene& scene, const ComponentRegistry& components, Entity entity, bool withDescendants) {
        Json entities = Json::array();
        WriteEntity(scene, components, entity, entities, withDescendants, WriteMode::Snapshot, nullptr);
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
            if (data.prefab.empty()) {
                scene.Remove<PrefabInstanceComponent>(entity);
            } else {
                scene.Add<PrefabInstanceComponent>(entity, data.prefab);
            }
        }
        // Liens aux prefabs une fois toutes les entités là : la racine de l'exemplaire peut suivre ses pièces.
        for (const EntityData& data : entities) {
            const Entity entity = scene.FindById(data.id);
            const Entity instance = data.source ? scene.FindById(data.instance) : NullEntity;
            if (instance != NullEntity) {
                scene.Add<PrefabLinkComponent>(entity, instance, data.source);
            } else {
                scene.Remove<PrefabLinkComponent>(entity);
            }
        }
        return {};
    }

    std::expected<std::string, std::string> SavePrefab(const Scene& scene, const ComponentRegistry& components, Entity root,
                                                       PrefabIds ids) {
        bool linked = false;
        std::function<void(Entity)> check = [&](Entity entity) {
            linked = linked || scene.TryGet<PrefabInstanceComponent>(entity) || scene.TryGet<PrefabLinkComponent>(entity);
            scene.ForEachChild(entity, check);
        };
        check(root);
        if (linked) {
            return std::unexpected("il contient déjà un prefab (un prefab dans un prefab n'est pas encore géré)");
        }
        Json entities = Json::array();
        WriteEntity(scene, components, root, entities, true, WriteMode::Prefab, nullptr);
        // Identifiants dans le prefab : ceux qui redonnent ceux de la scène pour l'exemplaire `root` (MemberId).
        const Uuid rootId = scene.GetId(root);
        const auto source = [&](const Json& id) { return MemberId(rootId, *Uuid::Parse(id.get<std::string>())).ToString(); };
        for (Json& entity : entities) {
            if (ids == PrefabIds::ForInstance) {
                entity["id"] = source(entity["id"]);
                if (entity.contains("parent")) {
                    entity["parent"] = source(entity["parent"]);
                }
            }
        }
        // Racine à l'origine : chaque exemplaire a sa place.
        Json& top = entities[0];
        top.erase("parent");
        top.erase("sector");
        if (top.contains("transform")) {
            top["transform"]["position"] = Json::array({0.0, 0.0, 0.0});
        }

        Json document;
        document["format"] = PrefabFormatName;
        document["version"] = FormatVersion;
        document["entities"] = std::move(entities);
        return CompactNumberArrays(document.dump(2)) + '\n';
    }

    void LinkPrefab(Scene& scene, Entity root, std::string_view path) {
        const Uuid rootId = scene.GetId(root);
        scene.Add<PrefabInstanceComponent>(root, std::string(path));
        std::function<void(Entity)> link = [&](Entity entity) {
            scene.Add<PrefabLinkComponent>(entity, root, MemberId(rootId, scene.GetId(entity)));
            scene.ForEachChild(entity, link);
        };
        link(root);
    }

    std::expected<Entity, std::string> Instantiate(Scene& scene, const ComponentRegistry& components, std::string_view path,
                                                   const PrefabReader& prefabs, Entity parent) {
        if (!prefabs) {
            return std::unexpected("aucun dossier d'assets où lire le prefab");
        }
        const auto text = prefabs(path);
        if (!text) {
            return std::unexpected(text.error());
        }
        std::vector<EntityData> entities;
        try {
            EntityData root;
            root.id = Uuid::Generate();
            root.prefab = std::string(path);
            if (parent != NullEntity) {
                root.parent = scene.GetId(parent);
            }
            ExpandInstance(components, *text, std::move(root), nullptr, entities);
        } catch (const std::exception& error) {
            return std::unexpected(std::format("prefab {} : {}", path, error.what()));
        }
        // Le nom de la racine du prefab (ExpandInstance garde celui de l'exemplaire, ici vide).
        try {
            entities[0].name = ReadPrefab(components, *text)[0].name;
        } catch (const std::exception&) {
        }
        for (const EntityData& data : entities) {
            if (scene.FindById(data.id) != NullEntity) {
                return std::unexpected("identifiant déjà pris dans la scène"); // une chance sur des milliards
            }
        }
        CreateEntities(scene, entities);
        return scene.FindById(entities[0].id);
    }

    namespace {
        // Pièce, chemin de son prefab et base du prefab ; nullptr si ce n'est pas une pièce.
        const PrefabLinkComponent* FindLink(const Scene& scene, Entity entity, std::string& path) {
            const auto* link = scene.TryGet<PrefabLinkComponent>(entity);
            if (!link || !scene.IsValid(link->instance)) {
                return nullptr;
            }
            const auto* instance = scene.TryGet<PrefabInstanceComponent>(link->instance);
            if (!instance) {
                return nullptr;
            }
            path = instance->prefab;
            return link;
        }
    }

    PrefabOverrides GetPrefabOverrides(const Scene& scene, const ComponentRegistry& components, Entity entity,
                                       const PrefabReader& prefabs) {
        PrefabOverrides result;
        std::string path;
        const PrefabLinkComponent* link = FindLink(scene, entity, path);
        if (!link) {
            return result;
        }
        PrefabTexts texts(prefabs);
        const std::unordered_map<Uuid, Baseline> baseline = ReadBaseline(components, texts, path);
        const auto base = baseline.find(link->source);
        if (base == baseline.end()) {
            return result;
        }
        result.known = true;
        Json current = Json::object();
        WriteComponents(scene, components, entity, current);
        const Json differences = Differences(components, current, scene.Get<NameComponent>(entity).name, &base->second,
                                             link->instance == entity);
        for (const auto& [key, value] : differences.items()) {
            if (key == "name") {
                result.name = true;
            } else if (key == "removed") {
                for (const Json& removed : value) {
                    result.removed.push_back(removed.get<std::string>());
                }
            } else if (!base->second.components.contains(key) || !components.Find(key)) {
                result.added.push_back(key);
            } else {
                for (const auto& [field, fieldValue] : value.items()) {
                    result.fields.emplace_back(key, field);
                }
            }
        }
        return result;
    }

    bool RevertToPrefab(Scene& scene, const ComponentRegistry& components, Entity entity, std::string_view key,
                        const PrefabReader& prefabs) {
        std::string path;
        const PrefabLinkComponent* link = FindLink(scene, entity, path);
        const ComponentInfo* info = components.Find(key);
        if (!link || !info) {
            return false;
        }
        PrefabTexts texts(prefabs);
        const std::unordered_map<Uuid, Baseline> baseline = ReadBaseline(components, texts, path);
        const auto base = baseline.find(link->source);
        if (base == baseline.end()) {
            return false;
        }
        const auto original = base->second.components.find(key);
        if (original == base->second.components.end()) {
            if (info->removable) {
                info->remove(scene.GetRegistry(), entity);
            }
            return true;
        }
        const std::shared_ptr<void> instance = info->create();
        try {
            for (const auto& [fieldKey, value] : original->items()) {
                if (const FieldInfo* field = info->FindField(fieldKey)) {
                    ReadField(*field, value, field->access(instance.get()));
                }
            }
        } catch (const std::exception&) {
            return false;
        }
        info->emplace(scene.GetRegistry(), entity, instance.get());
        return true;
    }

    void UnlinkPrefab(Scene& scene, Entity root) {
        scene.Remove<PrefabInstanceComponent>(root);
        std::function<void(Entity)> unlink = [&](Entity entity) {
            if (!IsMember(scene, entity, root)) {
                return;
            }
            scene.Remove<PrefabLinkComponent>(entity);
            scene.ForEachChild(entity, unlink);
        };
        unlink(root);
    }

    std::expected<void, std::string> WriteFile(const std::filesystem::path& path, std::string_view text) {
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

    std::expected<void, std::string> Save(const Scene& scene, const ComponentRegistry& components,
                                          const std::filesystem::path& path, const PrefabReader& prefabs) {
        return WriteFile(path, SaveToString(scene, components, prefabs));
    }

    std::expected<void, std::string> Load(Scene& scene, const ComponentRegistry& components,
                                          const std::filesystem::path& path, const PrefabReader& prefabs) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return std::unexpected("scène introuvable : " + Platform::ToUtf8(path));
        }
        std::ostringstream text;
        text << file.rdbuf();
        if (auto loaded = LoadFromString(scene, components, text.str(), prefabs); !loaded) {
            return std::unexpected(Platform::ToUtf8(path.filename()) + " : " + loaded.error());
        }
        return {};
    }
}
