#include "Scene/Registration/Registration.hpp"

#include "Scene/Photometry.hpp"

#include <algorithm>

// Rendu des objets : maillages, modèles, lumières.
namespace Flux {
    void RegisterRenderComponents(ComponentRegistry& registry) {
        registry.Register<MeshRendererComponent>("meshRenderer", "Rendu")
            .Enum("mesh", "Maillage", &MeshRendererComponent::mesh, {{"cube", "Cube", MeshType::Cube}})
            .Field("color", "Couleur", &MeshRendererComponent::color)
            .Field("luminance", "Lumière propre (cd/m²)", &MeshRendererComponent::luminance,
                   {.speed = 10.0, .min = 0.0, .max = 1.0e6, .decimals = 0})
            // Avant les unités physiques : 1 valait le blanc d'une surface en plein soleil. Pris comme l'émissif des
            // modèles glTF (1 : 1000 cd/m²), ce que des néons à 25 000 cd/m² auraient ébloui dans un intérieur.
            .ConvertedField("emission", [](MeshRendererComponent& mesh, double emission) {
                mesh.luminance = static_cast<float>(emission) * Photometry::NitsPerGltfEmissive;
            })
            .Field("metallic", "Métal", &MeshRendererComponent::metallic,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("roughness", "Rugosité", &MeshRendererComponent::roughness,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2});
        registry.Register<ModelComponent>("model", "Modèle 3D")
            .Asset("model", "Fichier", &ModelComponent::model, AssetType::Model)
            .Field("tint", "Teinte", &ModelComponent::tint)
            .Field("overrideMaterial", "Remplacer métal et rugosité", &ModelComponent::overrideMaterial)
            .Field("metallic", "Métal", &ModelComponent::metallic,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("roughness", "Rugosité", &ModelComponent::roughness,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Asset("farModel", "Version lointaine", &ModelComponent::farModel, AssetType::Model)
            .Field("farDistance", "Version lointaine au-delà de (m)", &ModelComponent::farDistance,
                   {.speed = 10.0, .min = 0.0, .max = 1.0e7, .decimals = 0});
        registry.Register<RimLightComponent>("rimLight", "Lumière de contour")
            .Field("color", "Couleur", &RimLightComponent::color, {.alpha = false})
            .Field("intensity", "Intensité", &RimLightComponent::intensity,
                   {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("sharpness", "Finesse", &RimLightComponent::sharpness,
                   {.speed = 0.05, .min = 0.5, .max = 32.0, .decimals = 1});
        registry.Register<LightComponent>("light", "Lumière")
            .Enum("type", "Type", &LightComponent::type,
                  {{"point", "Ponctuelle", LightType::Point}, {"spot", "Spot", LightType::Spot}})
            .Field("color", "Couleur", &LightComponent::color, {.alpha = false})
            .Field("lumens", "Flux (lumens)", &LightComponent::lumens,
                   {.speed = 10.0, .min = 0.0, .max = 1.0e8, .decimals = 0})
            // Avant les unités physiques : lumières réglées sur le plein soleil (5 à 1 m). La conversion à l'identique
            // (x 80 000 x 4π) donnerait des intérieurs éclairés comme en plein jour ; x 2500 est le rapport moyen entre
            // les anciennes valeurs des lampes de la station et leurs lumens réalistes (hangar 900 -> 2,25 millions,
            // couloir 8 -> 20 000), qui garde l'écart voulu entre une ampoule et un projecteur.
            .ConvertedField("intensity", [](LightComponent& light, double intensity) {
                light.lumens = static_cast<float>(std::max(intensity, 0.0) * 2500.0);
            })
            .Field("range", "Portée (m)", &LightComponent::range,
                   {.speed = 0.05, .min = 0.01, .max = 1.0e4, .decimals = 2})
            .Field("angle", "Ouverture du spot (°)", &LightComponent::angle,
                   {.speed = 0.5, .min = 1.0, .max = 179.0, .decimals = 1})
            .Field("softness", "Bord du spot adouci", &LightComponent::softness,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2});
    }
}
