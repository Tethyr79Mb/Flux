#include "Flux/Core/EditorExtensions.hpp"

#include <algorithm>

namespace Flux {
    void EditorExtensions::AddPlaceItem(PlaceItem item) { m_placeItems.push_back(std::move(item)); }

    const PlaceItem* EditorExtensions::FindPlaceItem(std::string_view label) const {
        const auto found = std::ranges::find(m_placeItems, label, &PlaceItem::label);
        return found != m_placeItems.end() ? &*found : nullptr;
    }

    void EditorExtensions::Clear() { m_placeItems.clear(); }
}
