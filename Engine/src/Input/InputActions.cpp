#include "Flux/Input/InputActions.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace Flux {
    namespace {
        using Json = nlohmann::ordered_json;

        constexpr std::string_view FormatName = "flux.input";
        constexpr int FormatVersion = 1;

        bool IsValidKey(Key key) {
            const auto scancode = static_cast<std::size_t>(key);
            return scancode > 0 && scancode < Input::KeyCount;
        }
    }

    void InputActions::Add(std::string name, std::string category, Key primary, Key secondary) {
        const InputActionKeys defaults{primary, secondary};
        const auto chosen = m_chosen.find(name);
        const InputActionKeys keys = chosen != m_chosen.end() ? chosen->second : defaults;
        if (InputAction* action = FindMutable(name)) {
            action->category = std::move(category);
            action->defaults = defaults;
            action->keys = keys;
            return;
        }
        m_actions.push_back({std::move(name), std::move(category), keys, defaults});
    }

    void InputActions::Remove(std::string_view name) {
        std::erase_if(m_actions, [name](const InputAction& action) { return action.name == name; });
    }

    const InputAction* InputActions::Find(std::string_view name) const {
        const auto found = std::ranges::find(m_actions, name, &InputAction::name);
        return found == m_actions.end() ? nullptr : &*found;
    }

    InputAction* InputActions::FindMutable(std::string_view name) {
        return const_cast<InputAction*>(std::as_const(*this).Find(name));
    }

    bool InputActions::IsDown(const Input& input, std::string_view name) const {
        const InputAction* action = Find(name);
        return action && std::ranges::any_of(action->keys, [&](Key key) { return IsValidKey(key) && input.IsKeyDown(key); });
    }

    bool InputActions::WasPressed(const Input& input, std::string_view name) const {
        const InputAction* action = Find(name);
        return action && std::ranges::any_of(action->keys, [&](Key key) { return IsValidKey(key) && input.WasKeyPressed(key); });
    }

    float InputActions::GetAxis(const Input& input, std::string_view positive, std::string_view negative) const {
        return (IsDown(input, positive) ? 1.0f : 0.0f) - (IsDown(input, negative) ? 1.0f : 0.0f);
    }

    bool InputActions::SetKey(std::string_view name, std::size_t slot, Key key) {
        InputAction* action = FindMutable(name);
        if (!action || slot >= InputActionKeyCount) {
            return false;
        }
        action->keys[slot] = key;
        if (action->keys == action->defaults) {
            if (const auto found = m_chosen.find(name); found != m_chosen.end()) {
                m_chosen.erase(found);
            }
        } else {
            m_chosen.insert_or_assign(action->name, action->keys);
        }
        return true;
    }

    void InputActions::ResetToDefaults() {
        m_chosen.clear();
        for (InputAction& action : m_actions) {
            action.keys = action.defaults;
        }
    }

    std::string InputActions::GetKeysText(std::string_view name) const {
        const InputAction* action = Find(name);
        std::string text;
        if (action) {
            for (const Key key : action->keys) {
                if (key == Key::Unknown) {
                    continue;
                }
                if (!text.empty()) {
                    text += " / ";
                }
                text += m_namer ? m_namer(key) : GetKeyName(key);
            }
        }
        return text.empty() ? "(aucune)" : text;
    }

    std::string InputActions::SaveToString() const {
        Json actions = Json::object();
        for (const auto& [name, keys] : m_chosen) {
            Json list = Json::array();
            for (const Key key : keys) {
                list.push_back(static_cast<int>(key)); // scancode : la même touche physique dans toutes les dispositions
            }
            actions[name] = std::move(list);
        }
        const Json json{{"format", FormatName}, {"version", FormatVersion}, {"actions", actions}};
        return json.dump(2);
    }

    std::expected<void, std::string> InputActions::LoadFromString(std::string_view text) {
        std::map<std::string, InputActionKeys, std::less<>> chosen;
        try {
            const Json json = Json::parse(text);
            if (json.value("format", std::string{}) != FormatName) {
                return std::unexpected("ce n'est pas un fichier de touches (format « flux.input » attendu)");
            }
            if (json.value("version", 0) > FormatVersion) {
                return std::unexpected("fichier de touches d'une version plus récente du moteur");
            }
            for (const auto& [name, list] : json.at("actions").items()) {
                if (!list.is_array()) {
                    return std::unexpected("touches de « " + name + " » : liste attendue");
                }
                InputActionKeys keys{};
                for (std::size_t slot = 0; slot < InputActionKeyCount && slot < list.size(); ++slot) {
                    const int scancode = list[slot].get<int>();
                    keys[slot] = scancode > 0 && scancode < static_cast<int>(Input::KeyCount) ? static_cast<Key>(scancode)
                                                                                                : Key::Unknown;
                }
                chosen.insert_or_assign(name, keys);
            }
        } catch (const std::exception& error) {
            return std::unexpected(std::string("fichier de touches illisible : ") + error.what());
        }
        m_chosen = std::move(chosen);
        for (InputAction& action : m_actions) {
            const auto found = m_chosen.find(action.name);
            action.keys = found != m_chosen.end() ? found->second : action.defaults;
        }
        return {};
    }
}
