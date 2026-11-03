// Outils de Claude pour les interfaces (.fluxui) : lister, ouvrir dans l'éditeur d'interface, lire l'arbre des
// éléments, ajouter, régler, déplacer, supprimer (un pas d'annulation par appel), enregistrer.
#include "Mcp/EditorTools.hpp"

#include "Interface/InterfaceEditor.hpp"
#include "Mcp/McpServer.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Project.hpp>
#include <Flux/Interface/Interface.hpp>
#include <Flux/Platform/Filesystem.hpp>

#include <algorithm>
#include <expected>
#include <format>
#include <functional>
#include <string>
#include <system_error>

namespace FluxEditor {
    using namespace Flux;
    using Json = nlohmann::json;

    namespace {
        Json Schema(Json properties, std::vector<std::string> required = {}) {
            Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
            if (!required.empty()) {
                schema["required"] = std::move(required);
            }
            return schema;
        }

        // Fichier relatif au dossier des assets, sans en sortir.
        std::expected<std::filesystem::path, std::string> AssetPath(const Engine& engine, const std::string& file) {
            const Project* project = engine.GetProject();
            if (!project) {
                return std::unexpected("aucun projet ouvert");
            }
            const std::filesystem::path relative = Platform::PathFromUtf8(file).lexically_normal();
            if (file.empty() || relative.has_root_path() || *relative.begin() == "..") {
                return std::unexpected(std::format("« {} » : chemin relatif au dossier des assets attendu", file));
            }
            if (relative.extension() != ".fluxui") {
                return std::unexpected(std::format("« {} » : fichier .fluxui attendu", file));
            }
            return project->assetsDirectory / relative;
        }

        // Clés du fichier (« window », « topLeft », « openWindow »...) : lues dans ce qu'écrit SaveToString, pour ne
        // pas en tenir une seconde liste ici.
        Json FileKeys(Interface::Element element, std::string_view key) {
            Interface::Document document;
            element.id = 1;
            document.roots.push_back(std::move(element));
            return Json::parse(Interface::SaveToString(document)).at("elements").at(0).value(std::string(key), Json());
        }

        // Élément d'id donné dans l'arbre JSON (« elements », puis « children ») et la liste qui le contient.
        Json* FindJson(Json& list, std::uint32_t id, Json** siblings = nullptr) {
            for (Json& element : list) {
                if (element.value("id", 0u) == id) {
                    if (siblings) {
                        *siblings = &list;
                    }
                    return &element;
                }
                if (const auto children = element.find("children"); children != element.end()) {
                    if (Json* found = FindJson(*children, id, siblings)) {
                        return found;
                    }
                }
            }
            return nullptr;
        }

        // Donne un identifiant neuf aux éléments ajoutés (et à leurs enfants) qui n'en ont pas.
        void AssignIds(Json& element, std::uint32_t& nextId, std::vector<std::uint32_t>& added) {
            if (!element.is_object()) {
                throw std::runtime_error("élément attendu (objet JSON)");
            }
            if (element.value("id", 0u) == 0) {
                element["id"] = nextId++;
            }
            added.push_back(element["id"].get<std::uint32_t>());
            if (const auto children = element.find("children"); children != element.end() && children->is_array()) {
                for (Json& child : *children) {
                    AssignIds(child, nextId, added);
                }
            }
        }

        // Liste où ranger un élément : la racine (parent 0) ou les enfants d'un conteneur.
        Json& TargetList(Json& file, std::uint32_t parent) {
            if (parent == 0) {
                return file["elements"];
            }
            Json* found = FindJson(file["elements"], parent);
            if (!found) {
                throw std::runtime_error(std::format("parent {} introuvable", parent));
            }
            if (!found->contains("children")) {
                (*found)["children"] = Json::array();
            }
            return (*found)["children"];
        }

        void Insert(Json& list, Json element, const Json& operation) {
            const std::size_t index = std::min<std::size_t>(operation.value("index", list.size()), list.size());
            list.insert(list.begin() + static_cast<std::ptrdiff_t>(index), std::move(element));
        }

        std::string Tree(const Interface::Document& document) {
            std::string text;
            const auto keys = Interface::GetTypeNames();
            std::function<void(const Interface::Element&, int)> visit = [&](const Interface::Element& element, int depth) {
                text += std::format("{}- {} {} « {} » ancre {} pos [{:g}, {:g}] taille [{:g}, {:g}]{}{}{}\n",
                                    std::string(static_cast<std::size_t>(depth) * 2, ' '), element.id,
                                    keys[static_cast<std::size_t>(element.type)], element.name,
                                    Interface::GetAnchorNames()[static_cast<std::size_t>(element.anchor)], element.position.x,
                                    element.position.y, element.size.x, element.size.y,
                                    element.text.empty() ? "" : std::format(" texte « {} »", element.text),
                                    element.binding.empty() ? "" : std::format(" liaison « {} »", element.binding),
                                    element.visible ? "" : " (caché)");
                for (const Interface::Element& child : element.children) {
                    visit(child, depth + 1);
                }
            };
            for (const Interface::Element& root : document.roots) {
                visit(root, 0);
            }
            return text.empty() ? std::string("(interface vide)\n") : text;
        }
    }

    void RegisterInterfaceTools(McpServer& server, EditorToolContext& context, InterfaceEditor& editor) {
        Engine& engine = context.engine;

        server.AddTool({
            .name = "list_interfaces",
            .description = "Interfaces (.fluxui) du dossier des assets, chemins relatifs ; dit laquelle est le menu du jeu "
                           "(Échap, Game.fluxproject) et laquelle est affichée dans l'éditeur d'interface.",
            .run = [&engine, &editor](const Json&) -> McpOutcome {
                const Project* project = engine.GetProject();
                if (!project) {
                    return McpToolResult::Error("aucun projet ouvert");
                }
                const std::filesystem::path* active = editor.GetActivePath();
                std::string text;
                std::error_code error;
                for (auto it = std::filesystem::recursive_directory_iterator(project->assetsDirectory, error);
                     !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
                    if (!it->is_regular_file(error) || it->path().extension() != ".fluxui") {
                        continue;
                    }
                    const std::string relative = Platform::ToGenericUtf8(it->path().lexically_relative(project->assetsDirectory));
                    text += std::format("{}{}{}\n", relative, relative == project->menu ? " [menu du jeu]" : "",
                                        active && active->lexically_normal() == it->path().lexically_normal() ? " [affichée]" : "");
                }
                return McpToolResult::Text(text.empty() ? std::string("aucune interface\n") : text);
            },
        });

        server.AddTool({
            .name = "open_interface",
            .description = "Ouvre une interface dans l'éditeur d'interface (onglet au-dessus de la vue) et l'affiche : les "
                           "autres outils d'interface agissent sur l'interface affichée. create : la crée vide si elle "
                           "n'existe pas. Rend l'arbre de ses éléments.",
            .inputSchema = Schema({{"file", {{"type", "string"}, {"description", "chemin relatif aux assets, .fluxui"}}},
                                   {"create", {{"type", "boolean"}}}},
                                  {"file"}),
            .run = [&engine, &editor](const Json& arguments) -> McpOutcome {
                const auto path = AssetPath(engine, arguments.value("file", std::string{}));
                if (!path) {
                    return McpToolResult::Error(path.error());
                }
                std::error_code error;
                if (arguments.value("create", false) && !std::filesystem::exists(*path, error)) {
                    std::filesystem::create_directories(path->parent_path(), error);
                    if (auto saved = Interface::SaveFile(Interface::Document{}, *path); !saved) {
                        return McpToolResult::Error(saved.error());
                    }
                }
                if (auto opened = editor.OpenFile(*path); !opened) {
                    return McpToolResult::Error(opened.error());
                }
                return McpToolResult::Text(Tree(*editor.GetActiveDocument()));
            },
        });

        server.AddTool({
            .name = "get_interface",
            .description = "Interface affichée. Sans element : l'arbre (id, type, nom, ancre, position, taille, texte, "
                           "liaison). Avec element (id) : cet élément au format du fichier (JSON), enfants compris.",
            .inputSchema = Schema({{"element", {{"type", "integer"}}}}),
            .run = [&editor](const Json& arguments) -> McpOutcome {
                const Interface::Document* document = editor.GetActiveDocument();
                if (!document) {
                    return McpToolResult::Error("aucune interface affichée : open_interface d'abord");
                }
                const std::uint32_t id = arguments.value("element", 0u);
                if (id == 0) {
                    return McpToolResult::Text(std::format("{}{}", editor.IsActiveModified() ? "(modifiée, pas enregistrée)\n" : "",
                                                           Tree(*document)));
                }
                Json file = Json::parse(Interface::SaveToString(*document));
                const Json* element = FindJson(file["elements"], id);
                return element ? McpToolResult::Text(element->dump(1)) : McpToolResult::Error(std::format("élément {} introuvable", id));
            },
        });

        server.AddTool({
            .name = "describe_interface",
            .description = "Ce qu'on peut écrire dans un élément : types, ancres, alignements, actions des boutons, champs "
                           "(clé, nom dans l'éditeur, types concernés, bornes) et valeurs liables (liaison).",
            .run = [&engine](const Json&) -> McpOutcome {
                std::string text = "Coordonnées en pixels d'un écran de 1080 lignes. Couleurs linéaires [r, g, b, a]. "
                                   "Ancre stretch : position = marges gauche et haut, size = marges droite et bas.\n"
                                   "Commun : id, type, name, anchor, position [x, y], size [w, h], visible, children "
                                   "(window, frame, tabs, scrollArea ; les enfants d'onglets sont leurs pages).\n\nTypes :";
                for (std::size_t i = 0; i < static_cast<std::size_t>(Interface::ElementType::Count); ++i) {
                    const auto type = static_cast<Interface::ElementType>(i);
                    text += std::format(" {} ({})", FileKeys(Interface::MakeDefault(type), "type").get<std::string>(),
                                        Interface::GetTypeNames()[i]);
                }
                text += "\nAncres :";
                for (std::size_t i = 0; i < static_cast<std::size_t>(Interface::Anchor::Count); ++i) {
                    Interface::Element element = Interface::MakeDefault(Interface::ElementType::Frame);
                    element.anchor = static_cast<Interface::Anchor>(i);
                    text += std::format(" {} ({})", FileKeys(element, "anchor").get<std::string>(), Interface::GetAnchorNames()[i]);
                }
                text += "\nAlignements (align) :";
                for (std::size_t i = 0; i < static_cast<std::size_t>(Interface::TextAlign::Count); ++i) {
                    Interface::Element element = Interface::MakeDefault(Interface::ElementType::Text);
                    element.align = static_cast<Interface::TextAlign>(i);
                    text += std::format(" {} ({})", FileKeys(element, "align").get<std::string>(), Interface::GetAlignNames()[i]);
                }
                text += "\nActions (action, cible dans target) :";
                for (std::size_t i = 0; i < static_cast<std::size_t>(Interface::ButtonAction::Count); ++i) {
                    Interface::Element element = Interface::MakeDefault(Interface::ElementType::Button);
                    element.action = static_cast<Interface::ButtonAction>(i);
                    text += std::format(" {} ({})", FileKeys(element, "action").get<std::string>(), Interface::GetActionNames()[i]);
                }
                text += "\n\nChamps :\n";
                for (const Interface::Field& field : Interface::GetFields()) {
                    std::string types;
                    for (std::size_t i = 0; i < static_cast<std::size_t>(Interface::ElementType::Count); ++i) {
                        if (field.AppliesTo(static_cast<Interface::ElementType>(i))) {
                            types += (types.empty() ? "" : ", ")
                                   + FileKeys(Interface::MakeDefault(static_cast<Interface::ElementType>(i)), "type").get<std::string>();
                        }
                    }
                    text += std::format("- {} : {} ({})", field.key, field.label, types);
                    if (std::holds_alternative<float Interface::Element::*>(field.member)) {
                        text += std::format(" [{:g} à {:g}]", field.min, field.max);
                    }
                    text += '\n';
                }
                text += "\nValeurs liables (binding) : texte « {} » remplacé par la valeur ; « Touche : <action> » donne "
                        "les touches d'une action (bouton avec action rebindKey, target = l'action).\n";
                for (const Engine::InterfaceValueInfo& value : engine.GetInterfaceValues()) {
                    text += std::format("- {} ({}){}\n", value.name, value.category,
                                        value.options.empty() ? "" : std::format(", {} choix", value.options.size()));
                }
                return McpToolResult::Text(std::move(text));
            },
        });

        server.AddTool({
            .name = "edit_interface",
            .description =
                "Modifie l'interface affichée : operations, appliquées dans l'ordre, forment un seul pas d'annulation "
                "(Ctrl+Z) ; si l'une échoue, rien ne change. Chaque opération : "
                "{\"op\": \"add\", \"parent\": id ou 0 (racine), \"index\": n facultatif, \"element\": élément au format "
                "du fichier, sans id (donné ici), enfants possibles ; les champs absents prennent la valeur par défaut du "
                "type} ; {\"op\": \"set\", \"id\": n, \"fields\": {\"text\": \"...\", \"size\": [w, h]...}} (seuls les "
                "champs donnés changent ; pas children) ; {\"op\": \"move\", \"id\": n, \"parent\": id ou 0, \"index\": n} ; "
                "{\"op\": \"remove\", \"id\": n}. Rend les id ajoutés et l'arbre. Pas enregistré : save_interface.",
            .inputSchema = Schema({{"operations", {{"type", "array"}, {"items", {{"type", "object"}}}}}}, {"operations"}),
            .run = [&editor](const Json& arguments) -> McpOutcome {
                const Interface::Document* document = editor.GetActiveDocument();
                if (!document) {
                    return McpToolResult::Error("aucune interface affichée : open_interface d'abord");
                }
                Json file = Json::parse(Interface::SaveToString(*document));
                std::uint32_t nextId = file.value("nextId", 1u);
                std::vector<std::uint32_t> added;
                std::size_t index = 0;
                try {
                    for (const Json& operation : arguments.at("operations")) {
                        const std::string op = operation.value("op", std::string{});
                        if (op == "add") {
                            Json element = operation.at("element");
                            AssignIds(element, nextId, added);
                            Insert(TargetList(file, operation.value("parent", 0u)), std::move(element), operation);
                        } else if (op == "set") {
                            Json* element = FindJson(file["elements"], operation.at("id").get<std::uint32_t>());
                            if (!element) {
                                throw std::runtime_error("élément introuvable");
                            }
                            for (const auto& [key, value] : operation.at("fields").items()) {
                                if (key == "id" || key == "children") {
                                    throw std::runtime_error(std::format("« {} » ne se change pas ici", key));
                                }
                                (*element)[key] = value;
                            }
                        } else if (op == "move" || op == "remove") {
                            const std::uint32_t id = operation.at("id").get<std::uint32_t>();
                            Json* siblings = nullptr;
                            Json* element = FindJson(file["elements"], id, &siblings);
                            if (!element) {
                                throw std::runtime_error("élément introuvable");
                            }
                            Json moving = std::move(*element);
                            siblings->erase(siblings->begin() + (element - &(*siblings)[0]));
                            if (op == "move") {
                                if (moving.contains("children") && FindJson(moving["children"], operation.value("parent", 0u))) {
                                    throw std::runtime_error("un élément ne va pas dans ses propres enfants");
                                }
                                Insert(TargetList(file, operation.value("parent", 0u)), std::move(moving), operation);
                            }
                        } else {
                            throw std::runtime_error(std::format("op inconnue : « {} » (add, set, move, remove)", op));
                        }
                        ++index;
                    }
                } catch (const std::exception& error) {
                    return McpToolResult::Error(std::format("opération {} : {} ; rien n'a changé", index + 1, error.what()));
                }
                file["nextId"] = nextId;
                auto loaded = Interface::LoadFromString(file.dump());
                if (!loaded) {
                    return McpToolResult::Error(std::format("interface refusée : {} ; rien n'a changé", loaded.error()));
                }
                editor.ReplaceActiveDocument(std::move(*loaded), added.empty() ? 0 : added.front());
                std::string ids;
                for (const std::uint32_t id : added) {
                    ids += (ids.empty() ? "" : ", ") + std::to_string(id);
                }
                return McpToolResult::Text(std::format("{} opération(s) ; ajoutés : {}\n{}", index, ids.empty() ? "aucun" : ids,
                                                       Tree(*editor.GetActiveDocument())));
            },
        });

        server.AddTool({
            .name = "save_interface",
            .description = "Enregistre l'interface affichée dans son fichier (comme Ctrl+S). Le jeu et le mode Jouer la "
                           "relisent aussitôt.",
            .run = [&editor](const Json&) -> McpOutcome {
                if (auto saved = editor.SaveActive(); !saved) {
                    return McpToolResult::Error(saved.error());
                }
                return McpToolResult::Text("enregistrée : " + Platform::ToGenericUtf8(editor.GetActivePath()->filename()));
            },
        });
    }
}
