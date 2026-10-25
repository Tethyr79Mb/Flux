#include "Core/InterfaceBindings.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Input/InputActions.hpp"

#include <cmath>
#include <format>

namespace Flux {
    namespace {
        // Liaison d'un texte ou d'un bouton aux touches d'une action d'entrée : « Touche : Avancer ».
        constexpr std::string_view KeyBindingPrefix = "Touche : ";
    }

    void InterfaceBindings::SetEngineValue(std::string name, Value value) {
        m_engineValues.insert_or_assign(std::move(name), std::move(value));
    }

    void InterfaceBindings::BindValue(std::string name, Value value) {
        m_gameValues.insert_or_assign(std::move(name), std::move(value));
    }

    void InterfaceBindings::UnbindValue(std::string_view name) {
        if (const auto found = m_gameValues.find(name); found != m_gameValues.end()) {
            m_gameValues.erase(found);
        }
    }

    std::vector<std::string> InterfaceBindings::GetValueNames(const InputActions& actions) const {
        std::vector<std::string> names;
        for (const auto& [name, value] : m_engineValues) {
            if (!m_gameValues.contains(name)) {
                names.push_back(name);
            }
        }
        for (const auto& [name, value] : m_gameValues) {
            names.push_back(name);
        }
        for (const InputAction& action : actions.GetActions()) {
            names.push_back(std::string(KeyBindingPrefix) + action.name);
        }
        return names;
    }

    void InterfaceBindings::RegisterFunction(std::string name, Engine::InterfaceFunction function) {
        m_functions.insert_or_assign(std::move(name), std::move(function));
    }

    void InterfaceBindings::UnregisterFunction(std::string_view name) {
        if (const auto found = m_functions.find(name); found != m_functions.end()) {
            m_functions.erase(found);
        }
    }

    void InterfaceBindings::ClearGame() {
        m_functions.clear();
        m_actions.clear();
        m_gameValues.clear();
    }

    const InterfaceBindings::Value* InterfaceBindings::Find(std::string_view name) const {
        if (const auto found = m_gameValues.find(name); found != m_gameValues.end()) {
            return &found->second; // celles du jeu d'abord : elles peuvent remplacer celles du moteur
        }
        const auto found = m_engineValues.find(name);
        return found != m_engineValues.end() ? &found->second : nullptr;
    }

    void InterfaceBindings::Sync(std::vector<Interface::Element>& elements, std::uint32_t changed, const InputActions& actions,
                                 std::string_view rebinding) const {
        for (Interface::Element& element : elements) {
            if (element.binding.starts_with(KeyBindingPrefix)) {
                // « Touche : Avancer » : les touches de l'action, sur un texte ou un bouton.
                const std::string_view action = std::string_view(element.binding).substr(KeyBindingPrefix.size());
                element.boundText = rebinding == action ? std::string("Appuyez sur une touche...")
                                                                : actions.GetKeysText(action);
            } else if (!element.binding.empty()) {
                if (const Value* value = Find(element.binding)) {
                    const bool touched = element.id == changed && value->set;
                    switch (element.type) {
                        case Interface::ElementType::Checkbox:
                            if (touched) {
                                value->set(element.checked ? 1.0 : 0.0);
                            } else if (value->get) {
                                element.checked = value->get() >= 0.5;
                            }
                            break;
                        case Interface::ElementType::Slider:
                            if (touched) {
                                value->set(element.value);
                            } else if (value->get) {
                                element.value = static_cast<float>(value->get());
                            }
                            break;
                        case Interface::ElementType::ProgressBar:
                            if (value->get) {
                                element.value = static_cast<float>(value->get());
                            }
                            break;
                        case Interface::ElementType::Text:
                        case Interface::ElementType::Button:
                            if (value->getText) {
                                element.boundText = value->getText();
                            } else if (value->get) {
                                // Entier sans décimales (images/s, munitions), sinon deux décimales.
                                const double number = value->get();
                                element.boundText = number == std::round(number) ? std::format("{:.0f}", number)
                                                                                 : std::format("{:.2f}", number);
                            }
                            break;
                        default:
                            break;
                    }
                }
            }
            Sync(element.children, changed, actions, rebinding);
        }
    }

    void InterfaceBindings::RunActions(Engine& engine, const Engine::InterfaceFunction& quit) {
        std::vector<std::pair<Interface::ButtonAction, std::string>> actions = std::exchange(m_actions, {});
        for (auto& [action, target] : actions) {
            if (engine.GetGameState() == GameState::Stopped) {
                return; // « Quitter » a arrêté le jeu (éditeur) : la suite visait le jeu
            }
            switch (action) {
                case Interface::ButtonAction::ChangeScene:
                    if (!target.empty()) {
                        engine.RequestScene(std::move(target));
                    }
                    break;
                case Interface::ButtonAction::Quit:
                    if (quit) {
                        quit(engine);
                    } else {
                        engine.RequestQuit();
                    }
                    break;
                case Interface::ButtonAction::RebindKey:
                    engine.StartKeyRebinding(std::move(target));
                    break;
                case Interface::ButtonAction::ResetKeys:
                    engine.ResetInputActions();
                    break;
                case Interface::ButtonAction::CallFunction:
                    if (const auto found = m_functions.find(target); found != m_functions.end()) {
                        // Copie : la fonction peut s'enregistrer à nouveau ou se retirer pendant l'appel.
                        const Engine::InterfaceFunction function = found->second;
                        function(engine);
                    } else {
                        Log::Warning("bouton d'interface : aucune fonction « {} » enregistrée par le jeu", target);
                    }
                    break;
                default:
                    break;
            }
        }
    }
}
