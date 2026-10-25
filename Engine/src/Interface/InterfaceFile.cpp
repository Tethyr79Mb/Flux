#include "Flux/Interface/Interface.hpp"

#include "Flux/Platform/Filesystem.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace Flux::Interface {
    namespace {
        using Json = nlohmann::ordered_json; // garde l'ordre des clés : fichiers stables et lisibles

        constexpr std::string_view FormatName = "flux.ui";
        constexpr int FormatVersion = 1;
        constexpr int MaxDepth = 64; // un fichier abîmé ou malveillant ne fait pas déborder la pile
        constexpr std::size_t MaxOptions = 256; // choix d'une liste déroulante

        constexpr std::array<std::string_view, static_cast<std::size_t>(ElementType::Count)> TypeKeys{
            "window", "frame", "text", "image", "button", "checkbox", "slider", "tabs", "progressBar", "scrollArea", "dropdown"};
        constexpr std::array<std::string_view, static_cast<std::size_t>(Anchor::Count)> AnchorKeys{
            "topLeft", "top", "topRight", "left", "center", "right", "bottomLeft", "bottom", "bottomRight", "stretch"};
        constexpr std::array<std::string_view, static_cast<std::size_t>(TextAlign::Count)> AlignKeys{"left", "center", "right"};
        constexpr std::array<std::string_view, static_cast<std::size_t>(ButtonAction::Count)> ActionKeys{
            "none", "openWindow", "closeWindow", "toggleWindow", "closeParent", "changeScene", "quit", "callFunction", "showTab", "rebindKey", "resetKeys"};

        // Écriture la plus courte qui redonne exactement ce float (« 0.1 » et non 0.10000000149011612).
        double Shortest(float value) {
            std::array<char, 32> buffer{};
            const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
            double result = 0.0;
            std::from_chars(buffer.data(), written.ptr, result);
            return result;
        }

        float Finite(float value) { return std::isfinite(value) ? std::clamp(value, -1.0e7f, 1.0e7f) : 0.0f; }

        template <std::size_t N>
        int IndexOf(const std::array<std::string_view, N>& keys, const std::string& key, std::string_view what) {
            const auto found = std::ranges::find(keys, key);
            if (found == keys.end()) {
                throw std::runtime_error(std::format("{} inconnu : « {} »", what, key));
            }
            return static_cast<int>(found - keys.begin());
        }

        Json ToJson(Math::Vector2f v) { return Json::array({Shortest(v.x), Shortest(v.y)}); }
        Json ToJson(const Math::LinearColor& c) { return Json::array({Shortest(c.r), Shortest(c.g), Shortest(c.b), Shortest(c.a)}); }

        Math::Vector2f ReadVector2(const Json& json) {
            if (!json.is_array() || json.size() != 2) {
                throw std::runtime_error("vecteur attendu : [x, y]");
            }
            return {Finite(json[0].get<float>()), Finite(json[1].get<float>())};
        }

        Math::LinearColor ReadColor(const Json& json) {
            if (!json.is_array() || json.size() != 4) {
                throw std::runtime_error("couleur attendue : [r, g, b, a]");
            }
            return {Finite(json[0].get<float>()), Finite(json[1].get<float>()), Finite(json[2].get<float>()),
                    std::clamp(Finite(json[3].get<float>()), 0.0f, 1.0f)};
        }

        Json SaveElement(const Element& element) {
            Json json{{"id", element.id},
                      {"type", TypeKeys[static_cast<std::size_t>(element.type)]},
                      {"name", element.name},
                      {"anchor", AnchorKeys[static_cast<std::size_t>(element.anchor)]},
                      {"position", ToJson(element.position)},
                      {"size", ToJson(element.size)}};
            if (!element.visible) {
                json["visible"] = false;
            }
            for (const Field& field : GetFields()) {
                if (!field.AppliesTo(element.type)) {
                    continue;
                }
                std::visit(
                    [&](auto member) {
                        const auto& value = element.*member;
                        using Value = std::remove_cvref_t<decltype(value)>;
                        if constexpr (std::is_same_v<Value, float>) {
                            json[std::string(field.key)] = Shortest(value);
                        } else if constexpr (std::is_same_v<Value, Math::LinearColor>) {
                            json[std::string(field.key)] = ToJson(value);
                        } else if constexpr (std::is_same_v<Value, TextAlign>) {
                            json[std::string(field.key)] = AlignKeys[static_cast<std::size_t>(value)];
                        } else if constexpr (std::is_same_v<Value, ButtonAction>) {
                            json[std::string(field.key)] = ActionKeys[static_cast<std::size_t>(value)];
                        } else {
                            json[std::string(field.key)] = value;
                        }
                    },
                    field.member);
            }
            if (!element.children.empty()) {
                Json children = Json::array();
                for (const Element& child : element.children) {
                    children.push_back(SaveElement(child));
                }
                json["children"] = std::move(children);
            }
            return json;
        }

        Element LoadElement(const Json& json, int depth, std::unordered_set<std::uint32_t>& ids) {
            if (depth > MaxDepth) {
                throw std::runtime_error("éléments imbriqués trop profondément");
            }
            if (!json.is_object()) {
                throw std::runtime_error("élément attendu");
            }
            const auto type = static_cast<ElementType>(IndexOf(TypeKeys, json.at("type").get<std::string>(), "type d'élément"));
            Element element = MakeDefault(type);
            element.id = json.at("id").get<std::uint32_t>();
            if (element.id == 0 || !ids.insert(element.id).second) {
                throw std::runtime_error(std::format("identifiant d'élément invalide ou en double : {}", element.id));
            }
            element.name = json.value("name", std::string{});
            element.anchor = static_cast<Anchor>(IndexOf(AnchorKeys, json.value("anchor", std::string("center")), "ancre"));
            if (const auto position = json.find("position"); position != json.end()) {
                element.position = ReadVector2(*position);
            }
            if (const auto size = json.find("size"); size != json.end()) {
                element.size = ReadVector2(*size);
            }
            element.visible = json.value("visible", true);
            for (const Field& field : GetFields()) {
                const auto found = json.find(field.key);
                if (!field.AppliesTo(type) || found == json.end()) {
                    continue; // réglage absent : celui par défaut du type
                }
                std::visit(
                    [&](auto member) {
                        auto& value = element.*member;
                        using Value = std::remove_cvref_t<decltype(value)>;
                        if constexpr (std::is_same_v<Value, float>) {
                            value = std::clamp(Finite(found->template get<float>()), field.min, field.max);
                        } else if constexpr (std::is_same_v<Value, Math::LinearColor>) {
                            value = ReadColor(*found);
                        } else if constexpr (std::is_same_v<Value, TextAlign>) {
                            value = static_cast<TextAlign>(IndexOf(AlignKeys, found->template get<std::string>(), "alignement"));
                        } else if constexpr (std::is_same_v<Value, ButtonAction>) {
                            value = static_cast<ButtonAction>(IndexOf(ActionKeys, found->template get<std::string>(), "action"));
                        } else if constexpr (std::is_same_v<Value, std::vector<std::string>>) {
                            if (!found->is_array() || found->size() > MaxOptions) {
                                throw std::runtime_error(std::format("« {} » : liste d'au plus {} textes attendue", field.key, MaxOptions));
                            }
                            value = found->template get<Value>();
                        } else {
                            value = found->template get<Value>();
                        }
                    },
                    field.member);
            }
            if (const auto children = json.find("children"); children != json.end()) {
                if (!children->is_array()) {
                    throw std::runtime_error("« children » doit être une liste");
                }
                for (const Json& child : *children) {
                    element.children.push_back(LoadElement(child, depth + 1, ids));
                }
            }
            return element;
        }
    }

    std::string SaveToString(const Document& document) {
        Json elements = Json::array();
        for (const Element& element : document.roots) {
            elements.push_back(SaveElement(element));
        }
        const Json json{{"format", FormatName}, {"version", FormatVersion}, {"nextId", document.nextId}, {"elements", elements}};
        return json.dump(2) + "\n";
    }

    std::expected<Document, std::string> LoadFromString(std::string_view text) {
        try {
            const Json json = Json::parse(text);
            if (json.value("format", std::string{}) != FormatName) {
                return std::unexpected("ce n'est pas une interface Flux (format « flux.ui » attendu)");
            }
            if (const int version = json.value("version", 0); version != FormatVersion) {
                return std::unexpected(std::format("version {} non prise en charge (attendue : {})", version, FormatVersion));
            }
            Document document;
            std::unordered_set<std::uint32_t> ids;
            for (const Json& element : json.at("elements")) {
                document.roots.push_back(LoadElement(element, 0, ids));
            }
            // Jamais un identifiant déjà pris, même si le fichier a été modifié à la main.
            const std::uint32_t highest = ids.empty() ? 0 : *std::ranges::max_element(ids);
            document.nextId = std::max(json.value("nextId", 1u), highest + 1);
            return document;
        } catch (const std::exception& error) {
            return std::unexpected(std::string(error.what()));
        }
    }

    std::expected<void, std::string> SaveFile(const Document& document, const std::filesystem::path& path) {
        const std::string text = SaveToString(document);
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

    std::expected<Document, std::string> LoadFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return std::unexpected("interface introuvable : " + Platform::ToUtf8(path));
        }
        std::ostringstream text;
        text << file.rdbuf();
        auto loaded = LoadFromString(text.str());
        if (!loaded) {
            return std::unexpected(Platform::ToUtf8(path.filename()) + " : " + loaded.error());
        }
        return loaded;
    }
}
