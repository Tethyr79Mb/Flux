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

    namespace {
        // Partagées par les éléments d'interface (texte, image) : mêmes noms dans les fichiers.
        const std::initializer_list<EnumValue<ScreenAnchor>> Anchors{
            {"topLeft", "En haut à gauche", ScreenAnchor::TopLeft},
            {"top", "En haut", ScreenAnchor::Top},
            {"topRight", "En haut à droite", ScreenAnchor::TopRight},
            {"left", "À gauche", ScreenAnchor::Left},
            {"center", "Au centre", ScreenAnchor::Center},
            {"right", "À droite", ScreenAnchor::Right},
            {"bottomLeft", "En bas à gauche", ScreenAnchor::BottomLeft},
            {"bottom", "En bas", ScreenAnchor::Bottom},
            {"bottomRight", "En bas à droite", ScreenAnchor::BottomRight},
        };
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
        registry.Register<ScreenTextComponent>("screenText", "Texte à l'écran")
            .Field("text", "Texte", &ScreenTextComponent::text)
            .Field("size", "Taille", &ScreenTextComponent::size, {.speed = 0.5, .min = 1.0, .max = 1000.0, .decimals = 0})
            .Field("color", "Couleur", &ScreenTextComponent::color)
            .Enum("anchor", "Ancre", &ScreenTextComponent::anchor, Anchors)
            .Field("offsetX", "Décalage X", &ScreenTextComponent::offsetX, {.speed = 1.0, .decimals = 0})
            .Field("offsetY", "Décalage Y", &ScreenTextComponent::offsetY, {.speed = 1.0, .decimals = 0});
        registry.Register<ScreenImageComponent>("screenImage", "Image à l'écran")
            .Asset("image", "Image", &ScreenImageComponent::image, AssetType::Image)
            .Field("tint", "Teinte", &ScreenImageComponent::tint)
            .Field("opacity", "Opacité", &ScreenImageComponent::opacity, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Enum("fit", "Remplissage", &ScreenImageComponent::fit,
                  {{"size", "Taille donnée", ScreenImageFit::Size},
                   {"stretch", "Étirée sur l'écran", ScreenImageFit::Stretch},
                   {"cover", "Couvre l'écran", ScreenImageFit::Cover}})
            .Field("width", "Largeur (0 : auto)", &ScreenImageComponent::width, {.speed = 1.0, .min = 0.0, .max = 100000.0, .decimals = 0})
            .Field("height", "Hauteur (0 : auto)", &ScreenImageComponent::height, {.speed = 1.0, .min = 0.0, .max = 100000.0, .decimals = 0})
            .Enum("anchor", "Ancre", &ScreenImageComponent::anchor, Anchors)
            .Field("offsetX", "Décalage X", &ScreenImageComponent::offsetX, {.speed = 1.0, .decimals = 0})
            .Field("offsetY", "Décalage Y", &ScreenImageComponent::offsetY, {.speed = 1.0, .decimals = 0});
    }
}
