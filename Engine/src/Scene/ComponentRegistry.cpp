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
            .Field("color", "Couleur", &MeshRendererComponent::color)
            .Field("emission", "Émission", &MeshRendererComponent::emission, {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("metallic", "Métal", &MeshRendererComponent::metallic, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("roughness", "Rugosité", &MeshRendererComponent::roughness, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2});
        registry.Register<ModelComponent>("model", "Modèle 3D")
            .Asset("model", "Fichier", &ModelComponent::model, AssetType::Model)
            .Field("tint", "Teinte", &ModelComponent::tint)
            .Field("overrideMaterial", "Remplacer métal et rugosité", &ModelComponent::overrideMaterial)
            .Field("metallic", "Métal", &ModelComponent::metallic, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("roughness", "Rugosité", &ModelComponent::roughness, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2});
        registry.Register<RimLightComponent>("rimLight", "Lumière de contour")
            .Field("color", "Couleur", &RimLightComponent::color, {.alpha = false})
            .Field("intensity", "Intensité", &RimLightComponent::intensity, {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("sharpness", "Finesse", &RimLightComponent::sharpness, {.speed = 0.05, .min = 0.5, .max = 32.0, .decimals = 1});
        registry.Register<LightComponent>("light", "Lumière")
            .Enum("type", "Type", &LightComponent::type,
                  {{"point", "Ponctuelle", LightType::Point}, {"spot", "Spot", LightType::Spot}})
            .Field("color", "Couleur", &LightComponent::color, {.alpha = false})
            .Field("intensity", "Intensité", &LightComponent::intensity, {.speed = 0.05, .min = 0.0, .max = 1.0e4, .decimals = 2})
            .Field("range", "Portée (m)", &LightComponent::range, {.speed = 0.05, .min = 0.01, .max = 1.0e4, .decimals = 2})
            .Field("angle", "Ouverture du spot (°)", &LightComponent::angle, {.speed = 0.5, .min = 1.0, .max = 179.0, .decimals = 1})
            .Field("softness", "Bord du spot adouci", &LightComponent::softness, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2});
        registry.Register<ThrusterComponent>("thruster", "Réacteur")
            .Field("color", "Couleur", &ThrusterComponent::color, {.alpha = false})
            .Field("width", "Largeur de la tuyère (m)", &ThrusterComponent::width, {.speed = 0.01, .min = 0.01, .max = 1.0e3, .decimals = 2})
            .Field("height", "Hauteur de la tuyère (m)", &ThrusterComponent::height, {.speed = 0.01, .min = 0.01, .max = 1.0e3, .decimals = 2})
            .Field("length", "Longueur à fond (m)", &ThrusterComponent::length, {.speed = 0.05, .min = 0.01, .max = 1.0e4, .decimals = 2})
            .Field("intensity", "Éclat", &ThrusterComponent::intensity, {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("idle", "Poussée au ralenti", &ThrusterComponent::idle, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("light", "Lumière à fond", &ThrusterComponent::light, {.speed = 0.05, .min = 0.0, .max = 1.0e4, .decimals = 2});
        const std::initializer_list<EnumValue<WeaponKind>> weaponKinds{{"gun", "Canon", WeaponKind::Gun},
                                                                       {"missile", "Missiles", WeaponKind::Missile}};
        registry.Register<HardpointComponent>("hardpoint", "Emport")
            .Enum("kind", "Accepte", &HardpointComponent::kind, weaponKinds)
            .Field("size", "Taille max (1 à 3)", &HardpointComponent::size, {.speed = 0.05, .min = 1.0, .max = 3.0, .decimals = 0});
        registry.Register<WeaponComponent>("weapon", "Arme")
            .Enum("kind", "Genre", &WeaponComponent::kind, weaponKinds)
            .Field("size", "Taille (1 à 3)", &WeaponComponent::size, {.speed = 0.05, .min = 1.0, .max = 3.0, .decimals = 0})
            .Field("group", "Groupe de tir (touches 1 à 9 ; 10 : touche 0)", &WeaponComponent::group, {.speed = 0.05, .min = 0.0, .max = 10.0, .decimals = 0})
            .Field("fireRate", "Cadence (tirs/s)", &WeaponComponent::fireRate, {.speed = 0.05, .min = 0.1, .max = 100.0, .decimals = 2})
            .Field("projectileSpeed", "Vitesse des projectiles (m/s)", &WeaponComponent::projectileSpeed,
                   {.speed = 5.0, .min = 10.0, .max = 1.0e5, .decimals = 0})
            .Field("range", "Portée (m)", &WeaponComponent::range, {.speed = 5.0, .min = 10.0, .max = 1.0e6, .decimals = 0})
            .Field("damage", "Dégâts", &WeaponComponent::damage, {.speed = 0.1, .min = 0.0, .max = 1.0e6, .decimals = 1})
            .Field("cone", "Débattement (°)", &WeaponComponent::cone, {.speed = 0.1, .min = 0.0, .max = 180.0, .decimals = 1})
            .Field("turnRate", "Pivotement (°/s)", &WeaponComponent::turnRate, {.speed = 1.0, .min = 0.0, .max = 3600.0, .decimals = 0})
            .Field("muzzle", "Bouche du canon (m)", &WeaponComponent::muzzle, {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("color", "Couleur des traçantes", &WeaponComponent::color, {.alpha = false})
            .Field("ammo", "Missiles emportés (0 : illimités)", &WeaponComponent::ammo, {.speed = 0.05, .min = 0.0, .max = 1000.0, .decimals = 0})
            .Field("guidance", "Missiles : virage (°/s)", &WeaponComponent::guidance, {.speed = 1.0, .min = 0.0, .max = 3600.0, .decimals = 0})
            .Field("lockAngle", "Verrouillage (°)", &WeaponComponent::lockAngle, {.speed = 0.1, .min = 0.0, .max = 90.0, .decimals = 1});
        registry.Register<HealthComponent>("health", "Points de vie")
            .Field("health", "Points de vie", &HealthComponent::health, {.speed = 1.0, .min = 0.0, .max = 1.0e9, .decimals = 0});
        registry.Register<SkyComponent>("sky", "Ciel")
            .Asset("image", "Panorama (vide : généré)", &SkyComponent::image, AssetType::Image)
            .Field("brightness", "Luminosité", &SkyComponent::brightness, {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("ambient", "Lumière ambiante", &SkyComponent::ambient, {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("seed", "Graine", &SkyComponent::seed, {.speed = 0.1, .min = 0.0, .max = 1.0e6, .decimals = 0})
            .Field("nebulaColor", "Nébuleuse", &SkyComponent::nebulaColor, {.alpha = false})
            .Field("nebulaColor2", "Nébuleuse (2e couleur)", &SkyComponent::nebulaColor2, {.alpha = false})
            .Field("nebula", "Densité de la nébuleuse", &SkyComponent::nebula, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("galaxy", "Bande galactique", &SkyComponent::galaxy, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("stars", "Étoiles", &SkyComponent::stars, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("starBrightness", "Éclat des étoiles", &SkyComponent::starBrightness, {.speed = 0.01, .min = 0.0, .max = 10.0, .decimals = 2})
            .Field("sunElevation", "Soleil : hauteur (°)", &SkyComponent::sunElevation, {.speed = 0.5, .min = -90.0, .max = 90.0, .decimals = 1})
            .Field("sunAzimuth", "Soleil : direction (°)", &SkyComponent::sunAzimuth, {.speed = 0.5, .min = -360.0, .max = 360.0, .decimals = 1})
            .Field("sunColor", "Soleil : couleur", &SkyComponent::sunColor, {.alpha = false})
            .Field("sunIntensity", "Soleil : intensité", &SkyComponent::sunIntensity, {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("sunSize", "Soleil : taille (°)", &SkyComponent::sunSize, {.speed = 0.05, .min = 0.0, .max = 45.0, .decimals = 2});
        registry.Register<WorldComponent>("world", "Monde")
            .Field("gravity", "Gravité (m/s²)", &WorldComponent::gravity, {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("air", "Air (1 : comme sur Terre)", &WorldComponent::air, {.speed = 0.01, .min = 0.0, .max = 10.0, .decimals = 2})
            .Field("terminalSpeed", "Chute max dans l'air (m/s)", &WorldComponent::terminalSpeed,
                   {.speed = 0.5, .min = 1.0, .max = 1.0e4, .decimals = 1});
        registry.Register<SunComponent>("sun", "Soleil")
            .Field("color", "Couleur du disque", &SunComponent::color, {.alpha = false})
            .Field("lightColor", "Couleur de la lumière", &SunComponent::lightColor, {.alpha = false})
            .Field("intensity", "Intensité", &SunComponent::intensity, {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("size", "Taille (°)", &SunComponent::size, {.speed = 0.01, .min = 0.0, .max = 45.0, .decimals = 2})
            .Enum("rays", "Rayons", &SunComponent::rays,
                  {{"star", "Étoile", SunRays::Star}, {"streak", "Trait horizontal", SunRays::Streak}, {"none", "Aucun", SunRays::None}});
        registry.Register<OrbitComponent>("orbit", "Orbite")
            .Field("distance", "Distance (millions de km)", &OrbitComponent::distance, {.speed = 0.1, .min = 0.0, .max = 1.0e5, .decimals = 3})
            .Field("angle", "Angle (°)", &OrbitComponent::angle, {.speed = 0.1, .min = -360.0, .max = 360.0, .decimals = 2})
            .Field("inclination", "Inclinaison (°)", &OrbitComponent::inclination, {.speed = 0.1, .min = -90.0, .max = 90.0, .decimals = 2});
        registry.Register<PlanetComponent>("planet", "Planète")
            .Field("radius", "Rayon (m)", &PlanetComponent::radius, {.speed = 100.0, .min = 100.0, .max = 1.0e7, .decimals = 0})
            .Field("relief", "Relief (m)", &PlanetComponent::relief, {.speed = 10.0, .min = 0.0, .max = 1.0e5, .decimals = 0})
            .Field("seed", "Graine", &PlanetComponent::seed, {.speed = 0.1, .min = 0.0, .max = 1.0e6, .decimals = 0})
            .Field("color", "Couleur de la roche", &PlanetComponent::color, {.alpha = false})
            .Field("roughness", "Rugosité de la roche", &PlanetComponent::roughness, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
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
            .Field("temperature", "Température à l'équateur (°C)", &PlanetComponent::temperature, {.speed = 0.5, .min = -200.0, .max = 200.0, .decimals = 1})
            .Field("poleTemperature", "Température aux pôles (°C)", &PlanetComponent::poleTemperature, {.speed = 0.5, .min = -200.0, .max = 200.0, .decimals = 1})
            .Field("humidity", "Humidité", &PlanetComponent::humidity, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("beachHeight", "Plage au-dessus de la mer (m)", &PlanetComponent::beachHeight, {.speed = 0.5, .min = 0.0, .max = 10000.0, .decimals = 1})
            .Field("snowLevel", "Neige à partir de (% du relief)", &PlanetComponent::snowLevel, {.speed = 0.5, .min = -100.0, .max = 200.0, .decimals = 1})
            // Hauteurs en mètres des premières versions de la PR #83, remplacées par les pourcentages.
            .RemovedField("sandHeight")
            .RemovedField("snowHeight")
            // Sable jusqu'à un % du relief au-dessus du niveau zéro : il ne suivait pas la mer quand on la baissait
            // (tout ce qui était entre les deux devenait du sable). Converti en hauteur au-dessus de la mer, qui
            // garde la même plage.
            .ConvertedField("sandLevel", [](PlanetComponent& planet, double sandLevel) {
                planet.beachHeight = std::max(static_cast<float>((sandLevel - planet.seaLevel) * 0.01 * planet.relief), 0.0f);
            })
            .Field("ocean", "Océan", &PlanetComponent::ocean)
            .Field("seaLevel", "Niveau de la mer (% du relief)", &PlanetComponent::seaLevel, {.speed = 0.1, .min = -100.0, .max = 100.0, .decimals = 2})
            .Field("waterColor", "Couleur de l'eau", &PlanetComponent::waterColor, {.alpha = false})
            .Field("gravity", "Gravité (m/s²)", &PlanetComponent::gravity, {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("atmosphere", "Atmosphère", &PlanetComponent::atmosphere, {.speed = 0.01, .min = 0.0, .max = 10.0, .decimals = 2})
            .Field("atmosphereHeight", "Hauteur de l'atmosphère (m)", &PlanetComponent::atmosphereHeight,
                   {.speed = 10.0, .min = 100.0, .max = 1.0e6, .decimals = 0})
            .Field("skyColor", "Couleur du ciel", &PlanetComponent::skyColor, {.alpha = false});
        registry.Register<CloudsComponent>("clouds", "Nuages")
            .Field("altitude", "Altitude (m)", &CloudsComponent::altitude, {.speed = 10.0, .min = 0.0, .max = 1.0e6, .decimals = 0})
            .Field("thickness", "Épaisseur (m)", &CloudsComponent::thickness, {.speed = 10.0, .min = 10.0, .max = 1.0e5, .decimals = 0})
            .Field("coverage", "Couverture", &CloudsComponent::coverage, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("opacity", "Opacité", &CloudsComponent::opacity, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("size", "Taille des formations (m)", &CloudsComponent::size, {.speed = 100.0, .min = 100.0, .max = 1.0e7, .decimals = 0})
            .Field("speed", "Vitesse (m/s)", &CloudsComponent::speed, {.speed = 0.1, .min = -1000.0, .max = 1000.0, .decimals = 1})
            .Field("color", "Couleur", &CloudsComponent::color, {.alpha = false});
        registry.Register<InteriorComponent>("interior", "Intérieur")
            .Field("size", "Taille (m)", &InteriorComponent::size, {.speed = 0.01, .min = 0.0, .max = 1.0e4})
            .Field("ambient", "Lumière ambiante", &InteriorComponent::ambient, {.alpha = false})
            .Field("intensity", "Intensité", &InteriorComponent::intensity, {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("fade", "Fondu aux bords (m)", &InteriorComponent::fade, {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2});
        registry.Register<ColliderComponent>("collider", "Collision")
            .Enum("shape", "Forme", &ColliderComponent::shape,
                  {{"box", "Boîte", ColliderShape::Box},
                   {"sphere", "Sphère", ColliderShape::Sphere},
                   {"capsule", "Capsule", ColliderShape::Capsule},
                   {"mesh", "Maillage", ColliderShape::Mesh}})
            .Field("size", "Taille (boîte)", &ColliderComponent::size, {.speed = 0.01, .min = 0.001, .max = 1.0e6})
            .Field("radius", "Rayon", &ColliderComponent::radius, {.speed = 0.01, .min = 0.001, .max = 1.0e6, .decimals = 2})
            .Field("height", "Hauteur (capsule)", &ColliderComponent::height, {.speed = 0.01, .min = 0.001, .max = 1.0e6, .decimals = 2})
            .Field("center", "Centre", &ColliderComponent::center, {.speed = 0.01})
            .Asset("mesh", "Maillage (vide : le modèle)", &ColliderComponent::mesh, AssetType::Model)
            .Field("simulatePhysics", "Simuler la physique", &ColliderComponent::simulatePhysics)
            .Field("cameraOnly", "Arrête seulement la caméra", &ColliderComponent::cameraOnly)
            .Field("mass", "Masse (kg, 0 : auto)", &ColliderComponent::mass, {.speed = 0.1, .min = 0.0, .max = 1.0e9, .decimals = 1})
            .Field("friction", "Frottement", &ColliderComponent::friction, {.speed = 0.01, .min = 0.0, .max = 10.0, .decimals = 2})
            .Field("restitution", "Rebond", &ColliderComponent::restitution, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("gravityFactor", "Gravité (x)", &ColliderComponent::gravityFactor, {.speed = 0.01, .min = -10.0, .max = 10.0, .decimals = 2});
        // Un personnage a sa propre capsule : une Collision sur la même entité serait ignorée par la physique.
        registry.Register<CharacterComponent>("character", "Personnage")
            .Excludes("collider")
            .Field("radius", "Rayon", &CharacterComponent::radius, {.speed = 0.01, .min = 0.05, .max = 10.0, .decimals = 2})
            .Field("height", "Taille", &CharacterComponent::height, {.speed = 0.01, .min = 0.2, .max = 20.0, .decimals = 2})
            .Field("walkSpeed", "Marche (m/s)", &CharacterComponent::walkSpeed, {.speed = 0.05, .min = 0.0, .max = 100.0, .decimals = 1})
            .Field("runSpeed", "Course (m/s)", &CharacterComponent::runSpeed, {.speed = 0.05, .min = 0.0, .max = 100.0, .decimals = 1})
            .Field("jumpSpeed", "Saut (m/s)", &CharacterComponent::jumpSpeed, {.speed = 0.05, .min = 0.0, .max = 100.0, .decimals = 1})
            .Field("player", "Dirigé par le joueur", &CharacterComponent::player);
        registry.Register<ShipComponent>("ship", "Vaisseau")
            .Field("gravity", "Gravité à bord (m/s²)", &ShipComponent::gravity, {.speed = 0.05, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("velocity", "Vitesse au départ (m/s)", &ShipComponent::velocity, {.speed = 0.1, .min = -1.0e5, .max = 1.0e5})
            .Field("angularVelocity", "Rotation au départ (°/s)", &ShipComponent::angularVelocity, {.speed = 0.5, .min = -720.0, .max = 720.0})
            .Field("maxSpeed", "Vitesse max (m/s)", &ShipComponent::maxSpeed, {.speed = 0.5, .min = 0.0, .max = 1.0e5, .decimals = 1})
            .Field("acceleration", "Accélération (m/s²)", &ShipComponent::acceleration, {.speed = 0.1, .min = 0.0, .max = 1.0e4, .decimals = 1})
            .Field("turnRate", "Virage (°/s)", &ShipComponent::turnRate, {.speed = 0.5, .min = 0.0, .max = 720.0, .decimals = 1})
            .Field("turnAcceleration", "Accélération du virage (°/s²)", &ShipComponent::turnAcceleration, {.speed = 1.0, .min = 0.0, .max = 1.0e4, .decimals = 1})
            .Field("strafeSpeed", "Glissement (m/s)", &ShipComponent::strafeSpeed, {.speed = 0.1, .min = 0.0, .max = 1.0e4, .decimals = 1})
            .Field("levelAssist", "Stabiliser à plat", &ShipComponent::levelAssist)
            .Field("levelRate", "Retour à plat (1/s)", &ShipComponent::levelRate, {.speed = 0.05, .min = 0.0, .max = 20.0, .decimals = 2})
            .Field("airGrip", "Tenue dans l'air (1/s)", &ShipComponent::airGrip, {.speed = 0.05, .min = 0.0, .max = 50.0, .decimals = 2})
            .Field("boost", "Boost (x)", &ShipComponent::boost, {.speed = 0.05, .min = 1.0, .max = 20.0, .decimals = 2})
            .Field("travelFactor", "Mode voyage (x)", &ShipComponent::travelFactor, {.speed = 0.1, .min = 1.0, .max = 1000.0, .decimals = 1})
            .Field("travelSpoolTime", "Mode voyage : mise en route (s)", &ShipComponent::travelSpoolTime, {.speed = 0.05, .min = 0.0, .max = 60.0, .decimals = 1})
            .Field("warpSpeed", "Warp : vitesse max (km/s)", &ShipComponent::warpSpeed, {.speed = 1000.0, .min = 0.0, .max = 1.0e9, .decimals = 0})
            .Field("fallsWithoutPilot", "Tombe sans pilote", &ShipComponent::fallsWithoutPilot);
        registry.Register<CombatAIComponent>("combatAI", "IA de combat")
            .Field("detectionRange", "Détection (m)", &CombatAIComponent::detectionRange, {.speed = 10.0, .min = 0.0, .max = 1.0e6, .decimals = 0})
            .Field("preferredRange", "Distance de combat (m)", &CombatAIComponent::preferredRange, {.speed = 5.0, .min = 10.0, .max = 1.0e5, .decimals = 0})
            .Field("aggression", "Agressivité (0 à 1)", &CombatAIComponent::aggression, {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("inaccuracy", "Imprécision (°)", &CombatAIComponent::inaccuracy, {.speed = 0.1, .min = 0.0, .max = 45.0, .decimals = 1})
            .Field("fleeHealth", "Fuite sous (% de vie)", &CombatAIComponent::fleeHealth, {.speed = 0.5, .min = 0.0, .max = 100.0, .decimals = 0})
            .Field("patrolRadius", "Rayon de patrouille (m)", &CombatAIComponent::patrolRadius, {.speed = 5.0, .min = 0.0, .max = 1.0e5, .decimals = 0});
        registry.Register<DockComponent>("dock", "Quai")
            .Field("approach", "Approche devant (m)", &DockComponent::approach, {.speed = 0.5, .min = 0.0, .max = 1.0e4, .decimals = 1})
            .Field("hover", "Hauteur d'arrivée (m)", &DockComponent::hover, {.speed = 0.1, .min = 0.0, .max = 1.0e3, .decimals = 1})
            .Field("clearance", "Passage au-dessus (m)", &DockComponent::clearance, {.speed = 0.5, .min = 0.0, .max = 1.0e4, .decimals = 1});
        registry.Register<MissionGiverComponent>("missionGiver", "Donneur de missions")
            .Field("title", "Titre", &MissionGiverComponent::title)
            .Field("briefing", "Ordre de mission", &MissionGiverComponent::briefing)
            .Asset("enemyPrefab", "Ennemis (prefab)", &MissionGiverComponent::enemyPrefab, AssetType::Prefab)
            .Field("enemyCount", "Nombre d'ennemis", &MissionGiverComponent::enemyCount, {.speed = 0.1, .min = 1.0, .max = 50.0, .decimals = 0})
            .Field("distance", "Distance (m)", &MissionGiverComponent::distance, {.speed = 100.0, .min = 0.0, .max = 1.0e8, .decimals = 0})
            .Field("spread", "Écart (m)", &MissionGiverComponent::spread, {.speed = 0.5, .min = 0.0, .max = 1.0e4, .decimals = 1})
            .Field("reach", "Portée pour parler (m)", &MissionGiverComponent::reach, {.speed = 0.05, .min = 0.5, .max = 20.0, .decimals = 2});
        registry.Register<SeatComponent>("seat", "Siège")
            .Field("pilot", "Poste de pilotage", &SeatComponent::pilot)
            .Field("eye", "Œil du pilote", &SeatComponent::eye, {.speed = 0.01})
            .Field("exit", "Sortie", &SeatComponent::exit, {.speed = 0.01})
            .Field("reach", "Portée (m)", &SeatComponent::reach, {.speed = 0.05, .min = 0.5, .max = 50.0});
        registry.Register<AnimatorComponent>("animator", "Animation")
            .Asset("animation", "Animation", &AnimatorComponent::animation, AssetType::Animation)
            .Asset("idleAnimation", "Au repos", &AnimatorComponent::idleAnimation, AssetType::Animation)
            .Asset("runAnimation", "En courant", &AnimatorComponent::runAnimation, AssetType::Animation)
            .Asset("airAnimation", "En l'air", &AnimatorComponent::airAnimation, AssetType::Animation)
            .Asset("sitDownAnimation", "S'asseoir", &AnimatorComponent::sitDownAnimation, AssetType::Animation)
            .Asset("seatedAnimation", "Assis", &AnimatorComponent::seatedAnimation, AssetType::Animation)
            .Asset("standUpAnimation", "Se lever", &AnimatorComponent::standUpAnimation, AssetType::Animation)
            .Field("speed", "Vitesse", &AnimatorComponent::speed, {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("loop", "En boucle", &AnimatorComponent::loop);
        registry.Register<ScreenTextComponent>("screenText", "Texte à l'écran")
            .Field("text", "Texte", &ScreenTextComponent::text)
            .Asset("font", "Police", &ScreenTextComponent::font, AssetType::Font)
            .Field("size", "Taille", &ScreenTextComponent::size, {.speed = 0.5, .min = 1.0, .max = 1000.0, .decimals = 0})
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
            .Field("minDistance", "Plein volume jusqu'à (m)", &SoundComponent::minDistance, {.speed = 0.05, .min = 0.0, .max = 1.0e6, .decimals = 2})
            .Field("maxDistance", "Silence au-delà de (m)", &SoundComponent::maxDistance, {.speed = 0.5, .min = 0.01, .max = 1.0e6, .decimals = 1})
            .Field("followShipSpeed", "Suit la vitesse du vaisseau", &SoundComponent::followShipSpeed);
        registry.Register<InterfaceComponent>("interface", "Interface à l'écran")
            .Asset("file", "Fichier", &InterfaceComponent::file, AssetType::Interface);
        registry.Register<SceneChangeComponent>("sceneChange", "Changement de scène")
            .Asset("scene", "Scène suivante", &SceneChangeComponent::scene, AssetType::Scene)
            .Field("delay", "Délai (s, 0 : aucun)", &SceneChangeComponent::delay, {.speed = 0.05, .min = 0.0, .max = 3600.0, .decimals = 2})
            .Field("onConfirm", "Sur clic ou touche", &SceneChangeComponent::onConfirm);
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
