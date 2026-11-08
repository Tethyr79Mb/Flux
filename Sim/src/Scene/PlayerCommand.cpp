#include "Flux/Scene/PlayerCommand.hpp"

namespace Flux {
    InputActionId CommandActions::Declare(std::string_view name) {
        if (const InputActionId id = GetId(name); id != NoInputAction) {
            return id;
        }
        if (m_ids.size() >= MaxInputActions) {
            return NoInputAction;
        }
        const auto id = static_cast<InputActionId>(m_ids.size());
        m_ids.emplace(std::string(name), id);
        return id;
    }

    InputActionId CommandActions::GetId(std::string_view name) const {
        const auto found = m_ids.find(name);
        return found != m_ids.end() ? found->second : NoInputAction;
    }
}
