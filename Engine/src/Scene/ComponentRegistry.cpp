#include "Flux/Scene/ComponentRegistry.hpp"

#include <algorithm>

namespace Flux {
    void ComponentRegistry::Unregister(std::string_view key) {
        std::erase_if(m_components, [&](const std::unique_ptr<ComponentInfo>& info) { return info->key == key; });
    }

    const ComponentInfo* ComponentRegistry::Find(std::string_view key) const {
        // Quelques dizaines de types au plus : une recherche linéaire suffit.
        const auto found = std::ranges::find_if(m_components, [&](const auto& info) { return info->key == key; });
        return found != m_components.end() ? found->get() : nullptr;
    }

    void ComponentRegistry::Truncate(std::size_t count) {
        if (count < m_components.size()) {
            m_components.resize(count);
        }
    }

    void RegisterEngineComponents(ComponentRegistry& registry) {
        registry.Register<TransformComponent>("transform", "Transformation")
            .Field("position", "Position", &TransformComponent::position, {.speed = 0.05})
            .Field("rotation", "Rotation (°)", &TransformComponent::rotation)
            .Field("scale", "Échelle", &TransformComponent::scale, {.speed = 0.01, .min = 0.001, .max = 1.0e6})
            .NotRemovable();
        registry.Register<MeshRendererComponent>("meshRenderer", "Rendu")
            .Enum("mesh", "Maillage", &MeshRendererComponent::mesh, {{"cube", "Cube", MeshType::Cube}})
            .Field("color", "Couleur", &MeshRendererComponent::color);
    }
}
