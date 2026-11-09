#include "Flux/Scene/ComponentRegistry.hpp"

#include "Scene/Registration/Registration.hpp"

#include <algorithm>

namespace Flux {
    void ComponentRegistry::Unregister(std::string_view key) {
        std::erase_if(m_components, [&](const std::unique_ptr<ComponentInfo>& info) { return info->key == key; });
        PruneMovedFields();
    }

    ComponentInfo* ComponentRegistry::FindMutable(std::string_view key) {
        const auto found = std::ranges::find_if(m_components, [&](const auto& info) { return info->key == key; });
        return found != m_components.end() ? found->get() : nullptr;
    }

    void ComponentRegistry::PruneMovedFields() {
        for (const auto& info : m_components) {
            std::erase_if(info->movedFields, [&](const ComponentInfo::MovedField& field) { return !Find(field.target); });
        }
    }

    const ComponentInfo* ComponentRegistry::Find(std::string_view key) const {
        // Quelques dizaines de types au plus : une recherche linéaire suffit.
        const auto found = std::ranges::find_if(m_components, [&](const auto& info) { return info->key == key; });
        return found != m_components.end() ? found->get() : nullptr;
    }

    void ComponentRegistry::Truncate(std::size_t count) {
        if (count < m_components.size()) {
            m_components.resize(count);
            PruneMovedFields();
        }
    }

    // L'ordre fixe celui de l'inspecteur et des composants dans les fichiers de scène.
    void RegisterEngineComponents(ComponentRegistry& registry) {
        registry.Register<TransformComponent>("transform", "Transformation")
            .Field("position", "Position", &TransformComponent::position, {.speed = 0.05})
            .Replicated()
            .Field("rotation", "Rotation (°)", &TransformComponent::rotation)
            .Replicated()
            .Field("scale", "Échelle", &TransformComponent::scale, {.speed = 0.01, .min = 0.001, .max = 1.0e6})
            .NotRemovable();
        RegisterRenderComponents(registry);
        RegisterPlanetComponents(registry);
        RegisterShipComponents(registry);
        RegisterCharacterComponents(registry);
        RegisterInterfaceComponents(registry);
    }
}
