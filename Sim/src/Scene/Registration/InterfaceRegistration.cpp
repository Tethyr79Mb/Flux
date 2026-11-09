#include "Scene/Registration/Registration.hpp"

// Interface à l'écran, sons et changements de scène.
namespace Flux {
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

    void RegisterInterfaceComponents(ComponentRegistry& registry) {
        registry.Register<ScreenTextComponent>("screenText", "Texte à l'écran")
            .Field("text", "Texte", &ScreenTextComponent::text)
            .Asset("font", "Police", &ScreenTextComponent::font, AssetType::Font)
            .Field("size", "Taille", &ScreenTextComponent::size,
                   {.speed = 0.5, .min = 1.0, .max = 1000.0, .decimals = 0})
            .Field("color", "Couleur", &ScreenTextComponent::color)
            .Enum("anchor", "Ancre", &ScreenTextComponent::anchor, Anchors)
            .Field("offsetX", "Décalage X", &ScreenTextComponent::offsetX, {.speed = 1.0, .decimals = 0})
            .Field("offsetY", "Décalage Y", &ScreenTextComponent::offsetY, {.speed = 1.0, .decimals = 0});
        registry.Register<SoundComponent>("sound", "Son")
            .Asset("file", "Fichier", &SoundComponent::file, AssetType::Sound)
            .Field("volume", "Volume", &SoundComponent::volume, {.speed = 0.01, .min = 0.0, .max = 10.0, .decimals = 2})
            .Field("pitch", "Hauteur", &SoundComponent::pitch, {.speed = 0.01, .min = 0.05, .max = 8.0, .decimals = 2})
            .Field("loop", "En boucle", &SoundComponent::loop)
            .Field("playOnStart", "Jouer au lancement", &SoundComponent::playOnStart)
            .Field("spatial", "Son 3D", &SoundComponent::spatial)
            .Field("minDistance", "Plein volume jusqu'à (m)", &SoundComponent::minDistance,
                   {.speed = 0.05, .min = 0.0, .max = 1.0e6, .decimals = 2})
            .Field("maxDistance", "Silence au-delà de (m)", &SoundComponent::maxDistance,
                   {.speed = 0.5, .min = 0.01, .max = 1.0e6, .decimals = 1})
            .Field("followShipSpeed", "Suit la vitesse du vaisseau", &SoundComponent::followShipSpeed);
        registry.Register<InterfaceComponent>("interface", "Interface à l'écran")
            .Asset("file", "Fichier", &InterfaceComponent::file, AssetType::Interface);
        registry.Register<SceneChangeComponent>("sceneChange", "Changement de scène")
            .Asset("scene", "Scène suivante", &SceneChangeComponent::scene, AssetType::Scene)
            .Field("delay", "Délai (s, 0 : aucun)", &SceneChangeComponent::delay,
                   {.speed = 0.05, .min = 0.0, .max = 3600.0, .decimals = 2})
            .Field("onConfirm", "Sur clic ou touche", &SceneChangeComponent::onConfirm);
        registry.Register<ScreenImageComponent>("screenImage", "Image à l'écran")
            .Asset("image", "Image", &ScreenImageComponent::image, AssetType::Image)
            .Field("tint", "Teinte", &ScreenImageComponent::tint)
            .Field("opacity", "Opacité", &ScreenImageComponent::opacity,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Enum("fit", "Remplissage", &ScreenImageComponent::fit,
                  {{"size", "Taille donnée", ScreenImageFit::Size},
                   {"stretch", "Étirée sur l'écran", ScreenImageFit::Stretch},
                   {"cover", "Couvre l'écran", ScreenImageFit::Cover}})
            .Field("width", "Largeur (0 : auto)", &ScreenImageComponent::width,
                   {.speed = 1.0, .min = 0.0, .max = 100000.0, .decimals = 0})
            .Field("height", "Hauteur (0 : auto)", &ScreenImageComponent::height,
                   {.speed = 1.0, .min = 0.0, .max = 100000.0, .decimals = 0})
            .Enum("anchor", "Ancre", &ScreenImageComponent::anchor, Anchors)
            .Field("offsetX", "Décalage X", &ScreenImageComponent::offsetX, {.speed = 1.0, .decimals = 0})
            .Field("offsetY", "Décalage Y", &ScreenImageComponent::offsetY, {.speed = 1.0, .decimals = 0});
    }
}
