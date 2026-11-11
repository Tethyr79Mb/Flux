#include "Mcp/EditorTools.hpp"

#include "Document/SceneDocument.hpp"
#include "Document/UndoHistory.hpp"
#include "Mcp/McpServer.hpp"
#include "Panels/PlacePanel.hpp"
#include "Panels/SceneEditor.hpp"

#include <Flux/Assets/Image.hpp>
#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Project.hpp>
#include <Flux/Graphics/Camera.hpp>
#include <Flux/Graphics/FlyCameraController.hpp>
#include <Flux/Math/Quaternion.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/Scene/ComponentRegistry.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/SceneSerializer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <unordered_map>

namespace FluxEditor {
    using namespace Flux;
    using Json = nlohmann::json;

    namespace {
        constexpr float DegreesPerRadian = 180.0f / std::numbers::pi_v<float>;
        // Clés d'une entité dans un instantané qui ne sont pas des composants.
        constexpr std::string_view EntityKeys[] = {"id", "name", "parent", "sector", "prefab", "prefabSource", "prefabInstance"};

        bool IsEntityKey(std::string_view key) {
            return std::ranges::contains(EntityKeys, key);
        }

        Json Schema(Json properties, std::vector<std::string> required = {}) {
            Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
            if (!required.empty()) {
                schema["required"] = std::move(required);
            }
            return schema;
        }

        const Json EntityArgument = {{"type", "string"}, {"description", "Uuid de l'objet, ou son nom exact"}};

        std::string_view FieldTypeName(FieldType type) {
            switch (type) {
                case FieldType::Bool: return "bool";
                case FieldType::Int: return "int";
                case FieldType::Float: return "float";
                case FieldType::Double: return "double";
                case FieldType::Vector3f: return "vec3 (float)";
                case FieldType::Vector3d: return "vec3 (double)";
                case FieldType::Quaternion: return "quaternion [x,y,z,w] (ou {\"degrees\":[x,y,z]})";
                case FieldType::Color: return "couleur linéaire [r,g,b,a]";
                case FieldType::String: return "texte";
                case FieldType::Enum: return "enum";
            }
            return "?";
        }

        // Objet désigné par son Uuid, sinon par son nom exact (le premier trouvé).
        std::expected<Entity, std::string> FindEntity(const Scene& scene, const Json& arguments, const char* key = "entity") {
            const auto found = arguments.find(key);
            if (found == arguments.end() || !found->is_string()) {
                return std::unexpected(std::format("« {} » manque (Uuid ou nom de l'objet)", key));
            }
            const std::string text = found->get<std::string>();
            if (const std::optional<Uuid> id = Uuid::Parse(text)) {
                if (const Entity entity = scene.FindById(*id); entity != NullEntity) {
                    return entity;
                }
            }
            if (const Entity entity = scene.FindByName(text); entity != NullEntity) {
                return entity;
            }
            return std::unexpected(std::format("aucun objet « {} » (list_entities pour les voir)", text));
        }

        // Pièce d'un exemplaire de prefab (pas sa racine) : le prefab la referait au chargement.
        bool IsPrefabMember(const Scene& scene, Entity entity) {
            const auto* link = scene.TryGet<PrefabLinkComponent>(entity);
            return link && link->instance != entity && scene.IsValid(link->instance);
        }

        std::string ComponentKeys(const Engine& engine, const Scene& scene, Entity entity) {
            const ComponentRegistry& components = const_cast<Engine&>(engine).GetComponents();
            std::string keys;
            for (std::size_t i = 0; i < components.GetCount(); ++i) {
                const ComponentInfo& info = components.Get(i);
                if (info.key != "transform" && info.get(scene.GetRegistry(), entity)) {
                    keys += keys.empty() ? info.key : ", " + info.key;
                }
            }
            return keys;
        }

        // Quaternion donné en degrés comme dans l'inspecteur (x : tangage, y : lacet, z : roulis) : converti au format
        // du fichier. Les autres valeurs sont gardées telles quelles.
        void ConvertDegrees(const ComponentInfo& info, Json& fields) {
            for (auto& [fieldKey, value] : fields.items()) {
                const FieldInfo* field = info.FindField(fieldKey);
                if (!field || field->type != FieldType::Quaternion || !value.is_object() || !value.contains("degrees")) {
                    continue;
                }
                const Json& degrees = value.at("degrees");
                const Math::Quaternionf q = Math::Normalize(Math::Quaternionf::FromYawPitchRoll(
                    degrees.at(1).get<float>() / DegreesPerRadian, degrees.at(0).get<float>() / DegreesPerRadian,
                    degrees.at(2).get<float>() / DegreesPerRadian));
                value = Json::array({q.x, q.y, q.z, q.w});
            }
        }

        // Applique des changements à l'instantané d'une entité (format du fichier de scène) puis le remet dans la scène,
        // sans toucher à l'historique. Rend des remarques sur les valeurs que le moteur a corrigées ou ignorées.
        std::expected<std::string, std::string> ApplyChanges(Engine& engine, Entity entity, const Json& changes) {
            if (!changes.is_object()) {
                return std::unexpected("« changes » doit être un objet");
            }
            Scene& scene = engine.GetScene();
            const ComponentRegistry& components = engine.GetComponents();
            Json snapshot = Json::parse(SceneSerializer::SaveEntity(scene, components, entity, false));
            Json& data = snapshot.at("entities").at(0);
            std::vector<std::pair<std::string, std::string>> asked; // (composant, champ) donnés
            for (const auto& [key, value] : changes.items()) {
                if (key == "id" || key == "prefab" || key == "prefabSource" || key == "prefabInstance") {
                    return std::unexpected(std::format("« {} » ne se change pas", key));
                }
                if (key == "parent" && IsPrefabMember(scene, entity)) {
                    return std::unexpected("pièce d'un exemplaire de prefab : on ne peut pas la sortir de son exemplaire");
                }
                if (IsEntityKey(key)) {
                    if (value.is_null()) {
                        data.erase(key); // parent : à la racine ; secteur : celui de l'origine
                    } else if (key == "parent" && value.is_string()) {
                        auto parent = FindEntity(scene, Json{{"parent", value}}, "parent");
                        if (!parent) {
                            return std::unexpected(parent.error());
                        }
                        data["parent"] = scene.GetId(*parent).ToString();
                    } else {
                        data[key] = value;
                    }
                    continue;
                }
                const ComponentInfo* info = components.Find(key);
                if (!info) {
                    return std::unexpected(std::format("composant inconnu : « {} » (describe_components pour la liste)", key));
                }
                if (value.is_null()) {
                    if (!info->removable) {
                        return std::unexpected(std::format("le composant « {} » ne se retire pas", key));
                    }
                    data.erase(key);
                    continue;
                }
                if (!value.is_object()) {
                    return std::unexpected(std::format("« {} » : un objet de champs attendu, ou null pour le retirer", key));
                }
                Json fields = value;
                for (const auto& [fieldKey, fieldValue] : fields.items()) {
                    if (!info->FindField(fieldKey)) {
                        std::string known;
                        for (const FieldInfo& field : info->fields) {
                            known += known.empty() ? field.key : ", " + field.key;
                        }
                        return std::unexpected(std::format("« {} » n'a pas de champ « {} » (champs : {})", key, fieldKey, known));
                    }
                    asked.emplace_back(key, fieldKey);
                }
                ConvertDegrees(*info, fields);
                Json& component = data[key];
                if (!component.is_object()) {
                    component = Json::object(); // ajouté : ses valeurs par défaut, puis celles données
                }
                component.update(fields);
            }
            if (auto restored = SceneSerializer::RestoreEntity(scene, components, snapshot.dump()); !restored) {
                return std::unexpected(restored.error());
            }

            // Relu : une valeur hors bornes ou d'un mauvais type a été corrigée ou ignorée par le moteur.
            const Json result = Json::parse(SceneSerializer::SaveEntity(scene, components, scene.FindById(scene.GetId(entity)), false));
            const Json& written = result.at("entities").at(0);
            std::string remarks;
            for (const auto& [key, fieldKey] : asked) {
                const Json& given = changes.at(key).at(fieldKey);
                const auto component = written.find(key);
                const Json now = component != written.end() && component->contains(fieldKey) ? component->at(fieldKey) : Json();
                if (given.is_object() || now == given) {
                    continue;
                }
                // Nombres : 1 et 1.0 sont la même valeur.
                if (given.is_number() && now.is_number() && std::abs(given.get<double>() - now.get<double>()) <= 1e-6 * std::max(1.0, std::abs(given.get<double>()))) {
                    continue;
                }
                remarks += std::format("\n{}.{} : demandé {}, obtenu {}", key, fieldKey, given.dump(), now.dump());
            }
            return remarks;
        }

        // Remplace chaque identifiant d'entité de l'instantané par un nouveau (parents, liens entre objets compris).
        void RenewIds(Json& value, const std::unordered_map<std::string, std::string>& ids) {
            if (value.is_string()) {
                if (const auto found = ids.find(value.get<std::string>()); found != ids.end()) {
                    value = found->second;
                }
            } else if (value.is_array()) {
                for (Json& child : value) {
                    RenewIds(child, ids);
                }
            } else if (value.is_object()) {
                for (auto& [key, child] : value.items()) {
                    // Identifiant de la pièce dans le fichier du prefab : il peut être celui de l'objet, mais ne change pas.
                    if (key != "prefabSource") {
                        RenewIds(child, ids);
                    }
                }
            }
        }

        std::string DescribeCamera(const Camera& camera, const FlyCameraController& controller) {
            const Math::Vector3f forward = camera.GetForward();
            return std::format("caméra : secteur [{}, {}, {}], position [{:.3f}, {:.3f}, {:.3f}] m, lacet {:.1f}°, tangage {:.1f}°, "
                               "champ vertical {:.0f}°, vitesse {:.3g} m/s, regarde [{:.3f}, {:.3f}, {:.3f}]",
                               camera.position.sector.x, camera.position.sector.y, camera.position.sector.z,
                               camera.position.offset.x, camera.position.offset.y, camera.position.offset.z,
                               camera.yaw * DegreesPerRadian, camera.pitch * DegreesPerRadian,
                               camera.verticalFov * DegreesPerRadian, controller.GetSpeed(), forward.x, forward.y, forward.z);
        }

        // Moyenne des pixels de chaque case : une capture réduite reste nette, sans crénelage.
        ImageData Shrink(const ImageData& source, int maxSize) {
            const int largest = std::max(source.size.x, source.size.y);
            if (largest <= maxSize) {
                return source;
            }
            const double scale = static_cast<double>(largest) / maxSize;
            ImageData target;
            target.size = {std::max(1, static_cast<int>(source.size.x / scale)), std::max(1, static_cast<int>(source.size.y / scale))};
            target.rgba.resize(static_cast<std::size_t>(target.size.x) * static_cast<std::size_t>(target.size.y) * 4);
            for (int y = 0; y < target.size.y; ++y) {
                const int y0 = static_cast<int>(y * scale);
                const int y1 = std::min(source.size.y, std::max(y0 + 1, static_cast<int>((y + 1) * scale)));
                for (int x = 0; x < target.size.x; ++x) {
                    const int x0 = static_cast<int>(x * scale);
                    const int x1 = std::min(source.size.x, std::max(x0 + 1, static_cast<int>((x + 1) * scale)));
                    std::uint32_t sum[4]{};
                    for (int sy = y0; sy < y1; ++sy) {
                        const std::uint8_t* row = source.rgba.data() + (static_cast<std::size_t>(sy) * source.size.x + x0) * 4;
                        for (int sx = x0; sx < x1; ++sx, row += 4) {
                            for (int c = 0; c < 4; ++c) {
                                sum[c] += row[c];
                            }
                        }
                    }
                    const std::uint32_t count = static_cast<std::uint32_t>((y1 - y0) * (x1 - x0));
                    std::uint8_t* out = target.rgba.data() + (static_cast<std::size_t>(y) * target.size.x + x) * 4;
                    for (int c = 0; c < 4; ++c) {
                        out[c] = static_cast<std::uint8_t>(sum[c] / count);
                    }
                }
            }
            return target;
        }

        ImageData Crop(const ImageData& source, int left, int top, int right, int bottom) {
            left = std::clamp(left, 0, source.size.x);
            right = std::clamp(right, left, source.size.x);
            top = std::clamp(top, 0, source.size.y);
            bottom = std::clamp(bottom, top, source.size.y);
            if (right - left <= 0 || bottom - top <= 0) {
                return source;
            }
            ImageData target;
            target.size = {right - left, bottom - top};
            target.rgba.resize(static_cast<std::size_t>(target.size.x) * static_cast<std::size_t>(target.size.y) * 4);
            for (int y = 0; y < target.size.y; ++y) {
                const std::uint8_t* from = source.rgba.data() + (static_cast<std::size_t>(top + y) * source.size.x + left) * 4;
                std::copy_n(from, static_cast<std::size_t>(target.size.x) * 4,
                            target.rgba.data() + static_cast<std::size_t>(y) * target.size.x * 4);
            }
            return target;
        }

        std::optional<McpToolResult> RefuseWhilePlaying(const EditorToolContext& context) {
            if (context.playing) {
                return McpToolResult::Error("mode Jouer : la scène du jeu est jetée à l'arrêt ; arrêter le jeu pour modifier");
            }
            return std::nullopt;
        }
    }

    void RegisterEditorTools(McpServer& server, EditorToolContext& context) {
        Engine& engine = context.engine;

        server.AddTool({
            .name = "engine_stats",
            .description = "Mesures du panneau Moteur, moyennes sur une demi-seconde : images/s, temps du CPU et du GPU "
                           "passe par passe (ms), exposition (EV100, luminance mesurée en cd/m²), objets dessinés, tuiles "
                           "et arbres des planètes, mémoire, réglages du rendu. Prend environ une seconde (temps du GPU "
                           "demandés le temps de la mesure). Les performances ne se jugent qu'en Release.",
            .run = [&engine](const Json&) -> McpOutcome {
                const auto start = std::chrono::steady_clock::now();
                return McpPoll([&engine, start]() -> std::optional<McpToolResult> {
                    // Deux fenêtres d'une demi-seconde : la seconde a les heures du GPU de bout en bout.
                    if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1100)) {
                        engine.GetRenderer().RequestDetailedTimings();
                        return std::nullopt;
                    }
                    return McpToolResult::Text(engine.GetStatsJson());
                });
            },
        });

        server.AddTool({
            .name = "list_entities",
            .description = "Arbre des objets de la scène ouverte : nom, Uuid et composants (sauf transform). filter : ne "
                           "garde que les objets dont le nom ou un composant contient ce texte (avec leurs ancêtres).",
            .inputSchema = Schema({{"filter", {{"type", "string"}}}}),
            .run = [&engine, &context](const Json& arguments) -> McpOutcome {
                const Scene& scene = engine.GetScene();
                const std::string filter = arguments.value("filter", std::string{});
                std::string text = std::format("{} objets{}\n", scene.GetEntityCount(),
                                               context.playing ? " (mode Jouer : copie de la scène)" : "");
                const Uuid selection = context.sceneEditor.GetSelection();
                std::size_t lines = 0;
                constexpr std::size_t MaxLines = 3000;
                std::function<bool(Entity, int)> visit = [&](Entity entity, int depth) -> bool {
                    const std::string& name = scene.Get<NameComponent>(entity).name;
                    const std::string keys = ComponentKeys(engine, scene, entity);
                    std::string children;
                    bool childMatches = false;
                    const std::size_t mark = text.size();
                    text += std::format("{}- {} {{{}}}{}{}{}\n", std::string(static_cast<std::size_t>(depth) * 2, ' '), name,
                                        scene.GetId(entity).ToString(), keys.empty() ? "" : " : ", keys,
                                        scene.GetId(entity) == selection ? " [sélectionné]" : "");
                    ++lines;
                    scene.ForEachChild(entity, [&](Entity child) { childMatches |= visit(child, depth + 1); });
                    const bool matches = filter.empty() || name.find(filter) != std::string::npos || keys.find(filter) != std::string::npos;
                    if (!matches && !childMatches) {
                        text.resize(mark);
                        --lines;
                    }
                    return matches || childMatches;
                };
                scene.ForEachRoot([&](Entity root) {
                    if (lines < MaxLines) {
                        visit(root, 0);
                    }
                });
                if (lines >= MaxLines) {
                    text += "... (liste coupée : utiliser filter)\n";
                }
                return McpToolResult::Text(std::move(text));
            },
        });

        server.AddTool({
            .name = "get_entity",
            .description = "Un objet au format du fichier de scène (JSON) : nom, parent, secteur, et chaque composant avec "
                           "tous ses champs. descendants : ses enfants aussi. Donne aussi sa position dans le monde.",
            .inputSchema = Schema({{"entity", EntityArgument}, {"descendants", {{"type", "boolean"}}}}, {"entity"}),
            .run = [&engine](const Json& arguments) -> McpOutcome {
                const Scene& scene = engine.GetScene();
                const auto entity = FindEntity(scene, arguments);
                if (!entity) {
                    return McpToolResult::Error(entity.error());
                }
                const Json snapshot = Json::parse(SceneSerializer::SaveEntity(scene, engine.GetComponents(), *entity,
                                                                              arguments.value("descendants", false)));
                const Math::WorldPosition world = scene.GetWorldPosition(*entity);
                return McpToolResult::Text(std::format("position dans le monde : secteur [{}, {}, {}], [{:.3f}, {:.3f}, {:.3f}] m\n{}",
                                                       world.sector.x, world.sector.y, world.sector.z, world.offset.x,
                                                       world.offset.y, world.offset.z, snapshot.at("entities").dump(1)));
            },
        });

        server.AddTool({
            .name = "describe_components",
            .description = "Sans argument : les composants qu'on peut mettre sur un objet (clé, nom dans l'inspecteur). "
                           "Avec component : ses champs (clé, type, nom, bornes, valeurs possibles d'une énumération).",
            .inputSchema = Schema({{"component", {{"type", "string"}}}}),
            .run = [&engine](const Json& arguments) -> McpOutcome {
                const ComponentRegistry& components = engine.GetComponents();
                const std::string key = arguments.value("component", std::string{});
                std::string text;
                if (key.empty()) {
                    for (std::size_t i = 0; i < components.GetCount(); ++i) {
                        const ComponentInfo& info = components.Get(i);
                        text += std::format("{} : {} ({} champs){}\n", info.key, info.label, info.fields.size(),
                                            info.removable ? "" : ", toujours présent");
                    }
                    return McpToolResult::Text(std::move(text));
                }
                const ComponentInfo* info = components.Find(key);
                if (!info) {
                    return McpToolResult::Error(std::format("composant inconnu : « {} »", key));
                }
                text = std::format("{} : {}\n", info->key, info->label);
                for (const FieldInfo& field : info->fields) {
                    text += std::format("- {} ({}) : {}", field.key, FieldTypeName(field.type), field.label);
                    if (field.options.min != std::numeric_limits<double>::lowest() || field.options.max != std::numeric_limits<double>::max()) {
                        text += std::format(" [{:g} à {:g}]", field.options.min, field.options.max);
                    }
                    if (field.type == FieldType::Enum) {
                        text += " ; valeurs :";
                        for (const EnumOption& option : field.enumOptions) {
                            text += std::format(" {} ({})", option.key, option.label);
                        }
                    }
                    if (field.asset != AssetType::None) {
                        text += " ; fichier relatif aux assets";
                    }
                    text += '\n';
                }
                return McpToolResult::Text(std::move(text));
            },
        });

        server.AddTool({
            .name = "edit_entity",
            .description = "Modifie un objet (annulable par Ctrl+Z, comme dans l'inspecteur). changes : objet au format du "
                           "fichier de scène, fusionné avec l'objet : {\"name\": \"...\"}, {\"parent\": \"Uuid\" ou null}, "
                           "{\"transform\": {\"position\": [x,y,z]}}, {\"light\": {\"intensity\": 5}} (ajoute le composant "
                           "s'il manque), {\"light\": null} (le retire). Seuls les champs donnés changent. Rend les "
                           "valeurs que le moteur a corrigées (bornes).",
            .inputSchema = Schema({{"entity", EntityArgument}, {"changes", {{"type", "object"}}}}, {"entity", "changes"}),
            .run = [&engine, &context](const Json& arguments) -> McpOutcome {
                if (auto refused = RefuseWhilePlaying(context)) {
                    return *refused;
                }
                Scene& scene = engine.GetScene();
                const auto entity = FindEntity(scene, arguments);
                if (!entity) {
                    return McpToolResult::Error(entity.error());
                }
                context.history.Commit(engine); // un geste en cours dans l'éditeur est un pas à part
                const Uuid id = scene.GetId(*entity);
                std::string before = UndoHistory::Capture(engine, id);
                auto applied = ApplyChanges(engine, *entity, arguments.at("changes"));
                if (!applied) {
                    return McpToolResult::Error(applied.error());
                }
                context.history.Push("Claude : modifier", id, std::move(before), UndoHistory::Capture(engine, id));
                return McpToolResult::Text("modifié" + *applied);
            },
        });

        server.AddTool({
            .name = "create_entity",
            .description = "Crée un objet (annulable). components : comme changes de edit_entity, par exemple "
                           "{\"meshRenderer\": {}, \"collider\": {}}. Sans position donnée ni parent, il est posé 5 m "
                           "devant la caméra. Rend son Uuid. Pour les objets tout faits (vaisseau, planète, prefab...), "
                           "voir place_item.",
            .inputSchema = Schema({{"name", {{"type", "string"}}},
                                   {"parent", EntityArgument},
                                   {"components", {{"type", "object"}}},
                                   {"select", {{"type", "boolean"}, {"description", "le sélectionner (vrai par défaut)"}}}},
                                  {"name"}),
            .run = [&engine, &context](const Json& arguments) -> McpOutcome {
                if (auto refused = RefuseWhilePlaying(context)) {
                    return *refused;
                }
                Scene& scene = engine.GetScene();
                Entity parent = NullEntity;
                if (arguments.contains("parent")) {
                    auto found = FindEntity(scene, arguments, "parent");
                    if (!found) {
                        return McpToolResult::Error(found.error());
                    }
                    parent = *found;
                }
                context.history.Commit(engine);
                const Entity entity = scene.CreateEntity(arguments.at("name").get<std::string>(), parent);
                const Json components = arguments.value("components", Json::object());
                const bool positioned = components.contains("transform") && components.at("transform").contains("position");
                if (parent == NullEntity && !positioned) {
                    const Camera& camera = engine.GetCamera();
                    Math::WorldPosition position = camera.position;
                    position += Math::Vector3d(camera.GetForward()) * 5.0;
                    scene.Get<TransformComponent>(entity).position = position.offset;
                    if (position.sector != Math::Vector3i64{}) {
                        scene.Add<SectorComponent>(entity, position.sector);
                    }
                }
                auto applied = ApplyChanges(engine, entity, components);
                if (!applied) {
                    scene.DestroyEntity(entity);
                    return McpToolResult::Error(applied.error());
                }
                const Uuid id = scene.GetId(entity);
                if (arguments.value("select", true)) {
                    context.sceneEditor.Select(scene, entity);
                }
                context.history.Push("Claude : créer", id, {}, UndoHistory::Capture(engine, id));
                return McpToolResult::Text(std::format("créé : {}{}", id.ToString(), *applied));
            },
        });

        server.AddTool({
            .name = "duplicate_entity",
            .description = "Duplique un objet avec ses enfants (annulable) ; la copie est placée juste après lui. "
                           "offset : déplacement de la copie en mètres [x,y,z]. Rend son Uuid.",
            .inputSchema = Schema({{"entity", EntityArgument},
                                   {"offset", {{"type", "array"}, {"items", {{"type", "number"}}}}}},
                                  {"entity"}),
            .run = [&engine, &context](const Json& arguments) -> McpOutcome {
                if (auto refused = RefuseWhilePlaying(context)) {
                    return *refused;
                }
                Scene& scene = engine.GetScene();
                const auto entity = FindEntity(scene, arguments);
                if (!entity) {
                    return McpToolResult::Error(entity.error());
                }
                if (IsPrefabMember(scene, *entity)) {
                    return McpToolResult::Error("pièce d'un exemplaire de prefab : dupliquer l'exemplaire entier (sa racine)");
                }
                context.history.Commit(engine);
                Json snapshot = Json::parse(SceneSerializer::SaveEntity(scene, engine.GetComponents(), *entity, true));
                std::unordered_map<std::string, std::string> ids;
                for (const Json& data : snapshot.at("entities")) {
                    ids.emplace(data.at("id").get<std::string>(), Uuid::Generate().ToString());
                }
                RenewIds(snapshot.at("entities"), ids);
                if (arguments.contains("offset")) {
                    const Json& offset = arguments.at("offset");
                    Json& transform = snapshot.at("entities").at(0)["transform"];
                    Json& position = transform["position"];
                    if (!position.is_array()) {
                        position = Json::array({0.0, 0.0, 0.0});
                    }
                    for (std::size_t i = 0; i < 3; ++i) {
                        position[i] = position[i].get<double>() + offset.at(i).get<double>();
                    }
                }
                if (auto restored = SceneSerializer::RestoreEntity(scene, engine.GetComponents(), snapshot.dump()); !restored) {
                    return McpToolResult::Error(restored.error());
                }
                const Uuid copy = *Uuid::Parse(ids.at(scene.GetId(*entity).ToString()));
                context.sceneEditor.Select(scene, scene.FindById(copy));
                context.history.Push("Claude : dupliquer", copy, {}, UndoHistory::Capture(engine, copy, true));
                return McpToolResult::Text("copie : " + copy.ToString());
            },
        });

        server.AddTool({
            .name = "delete_entity",
            .description = "Supprime un objet et ses enfants (annulable par Ctrl+Z).",
            .inputSchema = Schema({{"entity", EntityArgument}}, {"entity"}),
            .run = [&engine, &context](const Json& arguments) -> McpOutcome {
                if (auto refused = RefuseWhilePlaying(context)) {
                    return *refused;
                }
                Scene& scene = engine.GetScene();
                const auto entity = FindEntity(scene, arguments);
                if (!entity) {
                    return McpToolResult::Error(entity.error());
                }
                if (IsPrefabMember(scene, *entity)) {
                    return McpToolResult::Error("pièce d'un exemplaire de prefab : supprimer l'exemplaire entier, ou le délier");
                }
                context.history.Commit(engine);
                const Uuid id = scene.GetId(*entity);
                std::string before = UndoHistory::Capture(engine, id, true);
                if (context.sceneEditor.GetSelection() == id) {
                    context.sceneEditor.Select(scene, NullEntity);
                }
                scene.DestroyEntity(*entity);
                context.history.Push("Claude : supprimer", id, std::move(before), {});
                return McpToolResult::Text("supprimé");
            },
        });

        server.AddTool({
            .name = "select_entity",
            .description = "Sélectionne un objet dans l'éditeur (hiérarchie, inspecteur, contour dans la vue) pour le "
                           "montrer à Bruno ; sans entity, désélectionne.",
            .inputSchema = Schema({{"entity", EntityArgument}}),
            .run = [&engine, &context](const Json& arguments) -> McpOutcome {
                Scene& scene = engine.GetScene();
                if (!arguments.contains("entity")) {
                    context.sceneEditor.Select(scene, NullEntity);
                    return McpToolResult::Text("rien de sélectionné");
                }
                const auto entity = FindEntity(scene, arguments);
                if (!entity) {
                    return McpToolResult::Error(entity.error());
                }
                context.sceneEditor.Select(scene, *entity);
                return McpToolResult::Text("sélectionné");
            },
        });

        server.AddTool({
            .name = "undo",
            .description = "Annule le dernier pas de l'historique de l'éditeur (le sien ou celui de Bruno), comme Ctrl+Z ; "
                           "redo : rétablit, comme Ctrl+Y. Rend le nom du pas.",
            .inputSchema = Schema({{"redo", {{"type", "boolean"}}}}),
            .run = [&engine, &context](const Json& arguments) -> McpOutcome {
                if (auto refused = RefuseWhilePlaying(context)) {
                    return *refused;
                }
                UndoHistory& history = context.history;
                const bool redo = arguments.value("redo", false);
                history.Commit(engine);
                if (redo ? !history.CanRedo() : !history.CanUndo()) {
                    return McpToolResult::Error(redo ? "rien à rétablir" : "rien à annuler");
                }
                const std::string label = redo ? history.GetRedoLabel() : history.GetUndoLabel();
                const Uuid entity = redo ? history.Redo(engine) : history.Undo(engine);
                Scene& scene = engine.GetScene();
                context.sceneEditor.Select(scene, entity ? scene.FindById(entity) : NullEntity);
                return McpToolResult::Text((redo ? "rétabli : " : "annulé : ") + label);
            },
        });

        server.AddTool({
            .name = "camera",
            .description = "Caméra de l'éditeur. Sans argument : sa position et son orientation. position [x,y,z] m "
                           "(et sector [x,y,z], secteurs de 1e9 m), yaw/pitch en degrés (lacet 0 : regarde vers -z, positif "
                           "vers la gauche ; tangage positif : vers le haut), look_at : un objet à regarder, depuis "
                           "distance mètres (par défaut selon sa taille) si position n'est pas donnée.",
            .inputSchema = Schema({{"position", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                                   {"sector", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
                                   {"yaw", {{"type", "number"}}},
                                   {"pitch", {{"type", "number"}}},
                                   {"look_at", EntityArgument},
                                   {"distance", {{"type", "number"}}}}),
            .run = [&engine](const Json& arguments) -> McpOutcome {
                Camera& camera = engine.GetCamera();
                if (arguments.contains("sector")) {
                    const Json& sector = arguments.at("sector");
                    camera.position.sector = {sector.at(0).get<std::int64_t>(), sector.at(1).get<std::int64_t>(),
                                              sector.at(2).get<std::int64_t>()};
                }
                if (arguments.contains("position")) {
                    const Json& position = arguments.at("position");
                    camera.position.offset = {position.at(0).get<double>(), position.at(1).get<double>(), position.at(2).get<double>()};
                    camera.position.Normalize();
                }
                if (arguments.contains("yaw")) {
                    camera.yaw = arguments.at("yaw").get<float>() / DegreesPerRadian;
                }
                if (arguments.contains("pitch")) {
                    camera.pitch = std::clamp(arguments.at("pitch").get<float>(), -89.9f, 89.9f) / DegreesPerRadian;
                }
                if (arguments.contains("look_at")) {
                    Scene& scene = engine.GetScene();
                    const auto entity = FindEntity(scene, arguments, "look_at");
                    if (!entity) {
                        return McpToolResult::Error(entity.error());
                    }
                    const Math::WorldPosition target = scene.GetWorldPosition(*entity);
                    if (!arguments.contains("position")) {
                        double distance = arguments.value("distance", 0.0);
                        if (distance <= 0.0) {
                            // Assez loin pour le voir en entier (boîte du modèle et échelle de l'objet).
                            distance = 10.0;
                            if (const std::optional<Math::Box3f> bounds = engine.GetMeshBounds(*entity)) {
                                const Math::Vector3f size = bounds->max - bounds->min;
                                const Math::Vector3f scale = scene.Get<TransformComponent>(*entity).scale;
                                const Math::Vector3f scaled{size.x * scale.x, size.y * scale.y, size.z * scale.z};
                                const double radius = 0.5 * Math::Length(scaled);
                                distance = std::max(2.0, 2.5 * radius);
                            }
                        }
                        Math::WorldPosition from = target;
                        from += Math::Vector3d(camera.GetForward()) * -distance;
                        camera.position = from;
                    }
                    const Math::Vector3d direction = Math::Delta(camera.position, target);
                    const double length = Math::Length(direction);
                    if (length > 1e-6) {
                        camera.yaw = static_cast<float>(std::atan2(-direction.x, -direction.z));
                        camera.pitch = static_cast<float>(std::asin(std::clamp(direction.y / length, -1.0, 1.0)));
                    }
                }
                return McpToolResult::Text(DescribeCamera(camera, engine.GetCameraController()));
            },
        });

        server.AddTool({
            .name = "list_place_items",
            .description = "Objets du panneau Placer (ceux de l'éditeur, ceux du jeu, puis les prefabs du projet), à poser "
                           "avec place_item.",
            .run = [&engine, &context](const Json&) -> McpOutcome {
                std::string text;
                for (const std::string& label : PlacePanel::GetItemLabels(engine, *context.prefabs)) {
                    text += label + '\n';
                }
                return McpToolResult::Text(std::move(text));
            },
        });

        server.AddTool({
            .name = "place_item",
            .description = "Pose un objet du panneau Placer comme un clic sur sa ligne : devant la caméra (posé au sol si "
                           "le bouton de la vue est actif), sélectionné, annulable. item : son libellé exact "
                           "(list_place_items). file : pour « Modèle 3D... », « Personnage... », « Interface... », "
                           "« Son... », le fichier relatif aux assets. Rend son Uuid.",
            .inputSchema = Schema({{"item", {{"type", "string"}}}, {"file", {{"type", "string"}}}}, {"item"}),
            .run = [&engine, &context](const Json& arguments) -> McpOutcome {
                if (auto refused = RefuseWhilePlaying(context)) {
                    return *refused;
                }
                std::optional<std::string> file;
                if (arguments.contains("file")) {
                    file = arguments.at("file").get<std::string>();
                } else if (const std::string item = arguments.at("item").get<std::string>(); item.ends_with("...")) {
                    // Sans fichier, une boîte de dialogue s'ouvrirait et bloquerait l'éditeur jusqu'à ce que Bruno réponde.
                    return McpToolResult::Error(std::format("« {} » demande file (chemin relatif aux assets)", item));
                }
                auto placed = PlacePanel::PlaceByLabel(engine, context.sceneEditor, context.history,
                                                       arguments.at("item").get<std::string>(), std::move(file), context.view,
                                                       context.dropToGround, *context.prefabs);
                if (!placed) {
                    return McpToolResult::Error(placed.error());
                }
                return McpToolResult::Text("posé : " + placed->ToString());
            },
        });

        server.AddTool({
            .name = "open_scene",
            .description = "Ouvre une scène du projet (chemin relatif au dossier des assets, par exemple "
                           "\"scenes/Rendu/Ombres.scene.json\" : scènes de test du rendu dans scenes/Rendu). Sans "
                           "argument : la liste des scènes. Refusé si la scène ouverte a des modifications non "
                           "enregistrées, sauf discard true (elles sont perdues : seulement si Bruno l'a dit).",
            .inputSchema = Schema({{"scene", {{"type", "string"}}}, {"discard", {{"type", "boolean"}}}}),
            .run = [&engine, &context](const Json& arguments) -> McpOutcome {
                const Project* project = engine.GetProject();
                if (!project || !context.document) {
                    return McpToolResult::Error("aucun projet ouvert");
                }
                const std::string file = arguments.value("scene", std::string());
                if (file.empty()) {
                    std::string text;
                    std::error_code error;
                    for (auto it = std::filesystem::recursive_directory_iterator(project->assetsDirectory, error);
                         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
                        const std::string name = Platform::ToGenericUtf8(it->path().filename());
                        if (it->is_regular_file(error) && name.ends_with(".scene.json")) {
                            text += Platform::ToGenericUtf8(it->path().lexically_relative(project->assetsDirectory)) + "\n";
                        }
                    }
                    return McpToolResult::Text(text.empty() ? std::string("aucune scène") : text);
                }
                if (auto refused = RefuseWhilePlaying(context)) {
                    return *refused;
                }
                const std::filesystem::path relative = Platform::PathFromUtf8(file).lexically_normal();
                if (relative.has_root_path() || relative.empty() || *relative.begin() == "..") {
                    return McpToolResult::Error(std::format("« {} » : chemin relatif au dossier des assets attendu", file));
                }
                if (context.document->IsModified() && !arguments.value("discard", false)) {
                    return McpToolResult::Error("la scène ouverte a des modifications non enregistrées");
                }
                if (auto loaded = context.document->TryLoad(engine, project->assetsDirectory / relative); !loaded) {
                    return McpToolResult::Error(loaded.error());
                }
                context.sceneEditor.Select(engine.GetScene(), NullEntity);
                return McpToolResult::Text("scène ouverte : " + file);
            },
        });

        server.AddTool({
            .name = "debug_view",
            .description = "Vues de débogage du rendu (comme les cases du panneau Moteur). shadow_cascades : chaque pixel "
                           "teinté selon la carte d'ombre du soleil qu'il lit (rouge, vert, bleu, jaune : cascades proches "
                           "jusqu'à 6, 20, 60 et 200 m ; magenta et cyan : relief jusqu'à 3 et 40 km ; gris : aucune). "
                           "ray_shadows : ombres des modèles fixes en rayons (true, par défaut avec le ray tracing) ou dans "
                           "les cartes d'ombre (false), pour comparer. Sans argument : l'état actuel.",
            .inputSchema = Schema({{"shadow_cascades", {{"type", "boolean"}}}, {"ray_shadows", {{"type", "boolean"}}}}),
            .run = [&engine](const Json& arguments) -> McpOutcome {
                Renderer& renderer = engine.GetRenderer();
                if (arguments.contains("shadow_cascades")) {
                    renderer.SetShowShadowCascades(arguments.value("shadow_cascades", false));
                }
                if (arguments.contains("ray_shadows")) {
                    renderer.SetRayTracedShadows(arguments.value("ray_shadows", true));
                }
                return McpToolResult::Text(std::format("shadow_cascades : {}, ray_shadows : {}", renderer.IsShowingShadowCascades(),
                                                       renderer.IsRayTracingShadows()));
            },
        });

        server.AddTool({
            .name = "screenshot",
            .description = "Capture d'écran PNG de la vue 3D (region \"view\", par défaut) ou de toute la fenêtre de "
                           "l'éditeur (\"window\"), telle qu'à l'écran : grille, gizmo et contour de la sélection compris. "
                           "max_size : plus grand côté en pixels (1280 par défaut). delay_seconds : attendre avant (le temps "
                           "que les tuiles de planète ou les modèles se chargent après un déplacement de caméra).",
            .inputSchema = Schema({{"region", {{"type", "string"}, {"enum", {"view", "window"}}}},
                                   {"max_size", {{"type", "integer"}}},
                                   {"delay_seconds", {{"type", "number"}}}}),
            .run = [&engine, &context](const Json& arguments) -> McpOutcome {
                const bool window = arguments.value("region", std::string("view")) == "window";
                const int maxSize = std::clamp(arguments.value("max_size", 1280), 64, 4096);
                const double delay = std::clamp(arguments.value("delay_seconds", 0.0), 0.0, 30.0);
                const auto start = std::chrono::steady_clock::now();
                auto requested = std::make_shared<bool>(false);
                return McpPoll([&engine, &context, window, maxSize, delay, start, requested]() -> std::optional<McpToolResult> {
                    Renderer& renderer = engine.GetRenderer();
                    if (!*requested) {
                        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < delay) {
                            return std::nullopt;
                        }
                        if (!renderer.RequestCapture()) {
                            return McpToolResult::Error("la fenêtre ne permet pas la copie de son image (pilote)");
                        }
                        *requested = true;
                        return std::nullopt;
                    }
                    std::optional<ImageData> image = renderer.TakeCapture();
                    if (!image) {
                        return std::nullopt;
                    }
                    if (!window) {
                        const UI::Rect& view = context.view;
                        *image = Crop(*image, static_cast<int>(view.min.x), static_cast<int>(view.min.y),
                                      static_cast<int>(view.max.x), static_cast<int>(view.max.y));
                    }
                    const ImageData shrunk = Shrink(*image, maxSize);
                    const std::vector<std::uint8_t> png = EncodePng(shrunk);
                    McpToolResult result;
                    result.AddPng(std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
                    result.AddText(std::format("{} x {} pixels ({} x {} à l'écran) ; {}", shrunk.size.x, shrunk.size.y,
                                               image->size.x, image->size.y,
                                               DescribeCamera(engine.GetCamera(), engine.GetCameraController())));
                    return result;
                });
            },
        });
    }
}
