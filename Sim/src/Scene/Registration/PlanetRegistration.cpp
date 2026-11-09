#include "Scene/Registration/Registration.hpp"

#include "Scene/Photometry.hpp"

#include <algorithm>

// Ciel, soleil, orbites et planètes (relief, nuages, végétation).
namespace Flux {
    void RegisterPlanetComponents(ComponentRegistry& registry) {
        registry.Register<SkyComponent>("sky", "Ciel")
            .Asset("image", "Panorama (vide : généré)", &SkyComponent::image, AssetType::Image)
            .Field("luminance", "Luminosité du fond (cd/m²)", &SkyComponent::luminance,
                   {.speed = 0.01, .min = 0.0, .max = 1.0e5, .decimals = 3})
            // Avant les unités physiques : 1 pour le fond par défaut.
            .ConvertedField("brightness", [](SkyComponent& sky, double brightness) {
                sky.luminance = static_cast<float>(brightness) * SkyComponent{}.luminance;
            })
            .Field("ambient", "Lumière ambiante", &SkyComponent::ambient,
                   {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("seed", "Graine", &SkyComponent::seed, {.speed = 0.1, .min = 0.0, .max = 1.0e6, .decimals = 0})
            .Field("nebulaColor", "Nébuleuse", &SkyComponent::nebulaColor, {.alpha = false})
            .Field("nebulaColor2", "Nébuleuse (2e couleur)", &SkyComponent::nebulaColor2, {.alpha = false})
            .Field("nebula", "Densité de la nébuleuse", &SkyComponent::nebula,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("galaxy", "Bande galactique", &SkyComponent::galaxy,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("stars", "Étoiles", &SkyComponent::stars, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("starBrightness", "Éclat des étoiles", &SkyComponent::starBrightness,
                   {.speed = 0.01, .min = 0.0, .max = 10.0, .decimals = 2})
            .Field("sunElevation", "Soleil : hauteur (°)", &SkyComponent::sunElevation,
                   {.speed = 0.5, .min = -90.0, .max = 90.0, .decimals = 1})
            .Field("sunAzimuth", "Soleil : direction (°)", &SkyComponent::sunAzimuth,
                   {.speed = 0.5, .min = -360.0, .max = 360.0, .decimals = 1})
            .Field("sunColor", "Soleil : couleur", &SkyComponent::sunColor, {.alpha = false})
            .Field("sunIlluminance", "Soleil : éclairement (lux)", &SkyComponent::sunIlluminance,
                   {.speed = 100.0, .min = 0.0, .max = 1.0e7, .decimals = 0})
            .ConvertedField("sunIntensity", [](SkyComponent& sky, double intensity) {
                sky.sunIlluminance = static_cast<float>(intensity) * Photometry::LuxPerUnit;
            })
            .Field("sunSize", "Soleil : taille (°)", &SkyComponent::sunSize,
                   {.speed = 0.05, .min = 0.0, .max = 45.0, .decimals = 2})
            .Field("autoExposure", "Exposition automatique", &SkyComponent::autoExposure)
            .Field("exposure", "Exposition : décalage (stops)", &SkyComponent::exposure,
                   {.speed = 0.02, .min = -10.0, .max = 10.0, .decimals = 2})
            .Field("minEv100", "Exposition : scène la plus sombre (EV100)", &SkyComponent::minEv100,
                   {.speed = 0.05, .min = -10.0, .max = 25.0, .decimals = 1})
            .Field("maxEv100", "Exposition : scène la plus claire (EV100)", &SkyComponent::maxEv100,
                   {.speed = 0.05, .min = -10.0, .max = 25.0, .decimals = 1})
            // Bornes en stops autour de l'exposition 1, réglées pour l'ancienne échelle : elles interdisaient les
            // intérieurs éclairés par de vraies lampes. Les nouvelles bornes par défaut les remplacent.
            .RemovedField("exposureMin")
            .RemovedField("exposureMax")
            .Field("adaptationSpeed", "Exposition : vitesse d'adaptation", &SkyComponent::adaptationSpeed,
                   {.speed = 0.05, .min = 0.0, .max = 100.0, .decimals = 2});
        registry.Register<WorldComponent>("world", "Monde")
            .Field("gravity", "Gravité (m/s²)", &WorldComponent::gravity,
                   {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("air", "Air (1 : comme sur Terre)", &WorldComponent::air,
                   {.speed = 0.01, .min = 0.0, .max = 10.0, .decimals = 2})
            .Field("terminalSpeed", "Chute max dans l'air (m/s)", &WorldComponent::terminalSpeed,
                   {.speed = 0.5, .min = 1.0, .max = 1.0e4, .decimals = 1});
        registry.Register<SunComponent>("sun", "Soleil")
            .Field("color", "Couleur du disque", &SunComponent::color, {.alpha = false})
            .Field("lightColor", "Couleur de la lumière", &SunComponent::lightColor, {.alpha = false})
            .Field("illuminance", "Éclairement (lux)", &SunComponent::illuminance,
                   {.speed = 100.0, .min = 0.0, .max = 1.0e7, .decimals = 0})
            .ConvertedField("intensity", [](SunComponent& sun, double intensity) {
                sun.illuminance = static_cast<float>(intensity) * Photometry::LuxPerUnit;
            })
            .Field("size", "Taille (°)", &SunComponent::size, {.speed = 0.01, .min = 0.0, .max = 45.0, .decimals = 2})
            .Enum("rays", "Rayons", &SunComponent::rays,
                  {{"star", "Étoile", SunRays::Star},
                   {"streak", "Trait horizontal", SunRays::Streak},
                   {"none", "Aucun", SunRays::None}});
        registry.Register<OrbitComponent>("orbit", "Orbite")
            .Field("distance", "Distance (millions de km)", &OrbitComponent::distance,
                   {.speed = 0.1, .min = 0.0, .max = 1.0e5, .decimals = 3})
            .Field("angle", "Angle (°)", &OrbitComponent::angle,
                   {.speed = 0.1, .min = -360.0, .max = 360.0, .decimals = 2})
            .Field("inclination", "Inclinaison (°)", &OrbitComponent::inclination,
                   {.speed = 0.1, .min = -90.0, .max = 90.0, .decimals = 2});
        registry.Register<PlanetComponent>("planet", "Planète")
            .Group("relief", "Relief")
            .Field("radius", "Rayon (m)", &PlanetComponent::radius,
                   {.speed = 100.0, .min = 100.0, .max = 1.0e7, .decimals = 0})
            .Field("relief", "Relief (m)", &PlanetComponent::relief,
                   {.speed = 10.0, .min = 0.0, .max = 1.0e5, .decimals = 0})
            .Field("mountainWidth", "Largeur des montagnes (m)", &PlanetComponent::mountainWidth,
                   {.speed = 100.0, .min = 1000.0, .max = 1.0e6, .decimals = 0})
            .Field("seed", "Graine", &PlanetComponent::seed, {.speed = 0.1, .min = 0.0, .max = 1.0e6, .decimals = 0})
            .Group("ground", "Sol")
            .Field("color", "Couleur de la roche", &PlanetComponent::color, {.alpha = false})
            .Field("roughness", "Rugosité de la roche", &PlanetComponent::roughness,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("zones", "Herbe, sable et neige", &PlanetComponent::zones)
            .Field("grassColor", "Couleur de l'herbe", &PlanetComponent::grassColor, {.alpha = false})
            .Field("sandColor", "Couleur du sable", &PlanetComponent::sandColor, {.alpha = false})
            .Field("snowColor", "Couleur de la neige", &PlanetComponent::snowColor, {.alpha = false})
            .Field("soilColor", "Couleur de la terre", &PlanetComponent::soilColor, {.alpha = false})
            .Field("rockTint", "Teinte de la texture de roche", &PlanetComponent::rockTint, {.alpha = false})
            .Field("grassTint", "Teinte de la texture d'herbe", &PlanetComponent::grassTint, {.alpha = false})
            .Field("soilTint", "Teinte de la texture de terre", &PlanetComponent::soilTint, {.alpha = false})
            .Field("sandTint", "Teinte de la texture de sable", &PlanetComponent::sandTint, {.alpha = false})
            .Field("snowTint", "Teinte de la texture de neige", &PlanetComponent::snowTint, {.alpha = false})
            .Group("climate", "Climat")
            .Field("temperature", "Température à l'équateur (°C)", &PlanetComponent::temperature,
                   {.speed = 0.5, .min = -200.0, .max = 200.0, .decimals = 1})
            .Field("poleTemperature", "Température aux pôles (°C)", &PlanetComponent::poleTemperature,
                   {.speed = 0.5, .min = -200.0, .max = 200.0, .decimals = 1})
            .Field("humidity", "Humidité", &PlanetComponent::humidity,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("beachHeight", "Plage au-dessus de la mer (m)", &PlanetComponent::beachHeight,
                   {.speed = 0.5, .min = 0.0, .max = 10000.0, .decimals = 1})
            .Field("snowLevel", "Neige à partir de (% du relief)", &PlanetComponent::snowLevel,
                   {.speed = 0.5, .min = -100.0, .max = 200.0, .decimals = 1})
            // Hauteurs en mètres des premières versions de la PR #83, remplacées par les pourcentages.
            .RemovedField("sandHeight")
            .RemovedField("snowHeight")
            // Sable jusqu'à un % du relief au-dessus du niveau zéro : il ne suivait pas la mer quand on la baissait
            // (tout ce qui était entre les deux devenait du sable). Converti en hauteur au-dessus de la mer, qui
            // garde la même plage.
            .ConvertedField("sandLevel", [](PlanetComponent& planet, double sandLevel) {
                const double height = (sandLevel - planet.seaLevel) * 0.01 * planet.relief;
                planet.beachHeight = std::max(static_cast<float>(height), 0.0f);
            })
            .Group("ocean", "Océan")
            .Field("ocean", "Océan", &PlanetComponent::ocean)
            .Field("seaLevel", "Niveau de la mer (% du relief)", &PlanetComponent::seaLevel,
                   {.speed = 0.1, .min = -100.0, .max = 100.0, .decimals = 2})
            .Field("waterColor", "Couleur de l'eau", &PlanetComponent::waterColor, {.alpha = false})
            .Group("relief", "Relief")
            .Field("gravity", "Gravité (m/s²)", &PlanetComponent::gravity,
                   {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Group("atmosphere", "Atmosphère")
            .Field("atmosphere", "Atmosphère", &PlanetComponent::atmosphere,
                   {.speed = 0.01, .min = 0.0, .max = 10.0, .decimals = 2})
            .Field("atmosphereHeight", "Hauteur de l'atmosphère (m)", &PlanetComponent::atmosphereHeight,
                   {.speed = 10.0, .min = 100.0, .max = 1.0e6, .decimals = 0})
            .Field("skyColor", "Couleur du ciel", &PlanetComponent::skyColor, {.alpha = false});
        registry.Register<CloudsComponent>("clouds", "Nuages")
            .Field("altitude", "Altitude (m)", &CloudsComponent::altitude,
                   {.speed = 10.0, .min = 0.0, .max = 1.0e6, .decimals = 0})
            .Field("thickness", "Épaisseur (m)", &CloudsComponent::thickness,
                   {.speed = 10.0, .min = 10.0, .max = 1.0e5, .decimals = 0})
            .Field("coverage", "Couverture", &CloudsComponent::coverage,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("opacity", "Opacité", &CloudsComponent::opacity,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("size", "Taille des formations (m)", &CloudsComponent::size,
                   {.speed = 100.0, .min = 100.0, .max = 1.0e7, .decimals = 0})
            .Field("speed", "Vitesse (m/s)", &CloudsComponent::speed,
                   {.speed = 0.1, .min = -1000.0, .max = 1000.0, .decimals = 1})
            .Field("color", "Couleur", &CloudsComponent::color, {.alpha = false});
        registry.Register<VegetationComponent>("vegetation", "Végétation")
            .Asset("biome1", "Biome 1", &VegetationComponent::biome1, AssetType::Biome)
            .Asset("biome2", "Biome 2", &VegetationComponent::biome2, AssetType::Biome)
            .Asset("biome3", "Biome 3", &VegetationComponent::biome3, AssetType::Biome)
            .Asset("biome4", "Biome 4", &VegetationComponent::biome4, AssetType::Biome)
            .Asset("biome5", "Biome 5", &VegetationComponent::biome5, AssetType::Biome)
            .Asset("biome6", "Biome 6", &VegetationComponent::biome6, AssetType::Biome)
            .Asset("biome7", "Biome 7", &VegetationComponent::biome7, AssetType::Biome)
            .Asset("biome8", "Biome 8", &VegetationComponent::biome8, AssetType::Biome)
            // Avant les biomes : un seul arbre et une seule forêt pour toute la planète.
            .RemovedField("model")
            .RemovedField("lodModel")
            .RemovedField("farModel")
            .RemovedField("density")
            .RemovedField("cover")
            .RemovedField("maxSlope")
            .RemovedField("minScale")
            .RemovedField("maxScale")
            .Field("lodDistance", "Simplifié au-delà de (m)", &VegetationComponent::lodDistance,
                   {.speed = 1.0, .min = 0.0, .max = 1.0e5, .decimals = 0})
            .Field("farDistance", "Lointain au-delà de (m)", &VegetationComponent::farDistance,
                   {.speed = 1.0, .min = 0.0, .max = 1.0e5, .decimals = 0})
            .Field("range", "Visibles jusqu'à (m)", &VegetationComponent::range,
                   {.speed = 10.0, .min = 0.0, .max = 1.0e5, .decimals = 0})
            .Field("leafWrap", "Lumière enveloppante", &VegetationComponent::leafWrap,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("leafTransmission", "Lumière traversante", &VegetationComponent::leafTransmission,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2});
    }
}
