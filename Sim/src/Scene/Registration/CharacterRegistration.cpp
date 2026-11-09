#include "Scene/Registration/Registration.hpp"

// Collisions, personnages et animations.
namespace Flux {
    void RegisterCharacterComponents(ComponentRegistry& registry) {
        registry.Register<ColliderComponent>("collider", "Collision")
            .Enum("shape", "Forme", &ColliderComponent::shape,
                  {{"box", "Boîte", ColliderShape::Box},
                   {"sphere", "Sphère", ColliderShape::Sphere},
                   {"capsule", "Capsule", ColliderShape::Capsule},
                   {"mesh", "Maillage", ColliderShape::Mesh}})
            .Field("size", "Taille (boîte)", &ColliderComponent::size, {.speed = 0.01, .min = 0.001, .max = 1.0e6})
            .Field("radius", "Rayon", &ColliderComponent::radius,
                   {.speed = 0.01, .min = 0.001, .max = 1.0e6, .decimals = 2})
            .Field("height", "Hauteur (capsule)", &ColliderComponent::height,
                   {.speed = 0.01, .min = 0.001, .max = 1.0e6, .decimals = 2})
            .Field("center", "Centre", &ColliderComponent::center, {.speed = 0.01})
            .Asset("mesh", "Maillage (vide : le modèle)", &ColliderComponent::mesh, AssetType::Model)
            .Field("simulatePhysics", "Simuler la physique", &ColliderComponent::simulatePhysics)
            .Field("cameraOnly", "Arrête seulement la caméra", &ColliderComponent::cameraOnly)
            .Field("mass", "Masse (kg, 0 : auto)", &ColliderComponent::mass,
                   {.speed = 0.1, .min = 0.0, .max = 1.0e9, .decimals = 1})
            .Field("friction", "Frottement", &ColliderComponent::friction,
                   {.speed = 0.01, .min = 0.0, .max = 10.0, .decimals = 2})
            .Field("restitution", "Rebond", &ColliderComponent::restitution,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("gravityFactor", "Gravité (x)", &ColliderComponent::gravityFactor,
                   {.speed = 0.01, .min = -10.0, .max = 10.0, .decimals = 2});
        // Un personnage a sa propre capsule : une Collision sur la même entité serait ignorée par la physique.
        registry.Register<CharacterComponent>("character", "Personnage")
            .Excludes("collider")
            .Field("radius", "Rayon", &CharacterComponent::radius,
                   {.speed = 0.01, .min = 0.05, .max = 10.0, .decimals = 2})
            .Field("height", "Taille", &CharacterComponent::height,
                   {.speed = 0.01, .min = 0.2, .max = 20.0, .decimals = 2})
            .Field("walkSpeed", "Marche (m/s)", &CharacterComponent::walkSpeed,
                   {.speed = 0.05, .min = 0.0, .max = 100.0, .decimals = 1})
            .Field("runSpeed", "Course (m/s)", &CharacterComponent::runSpeed,
                   {.speed = 0.05, .min = 0.0, .max = 100.0, .decimals = 1})
            .Field("jumpSpeed", "Saut (m/s)", &CharacterComponent::jumpSpeed,
                   {.speed = 0.05, .min = 0.0, .max = 100.0, .decimals = 1})
            .Field("acceleration", "Accélération (m/s²)", &CharacterComponent::acceleration,
                   {.speed = 0.1, .min = 0.5, .max = 1000.0, .decimals = 1})
            .Field("player", "Dirigé par le joueur", &CharacterComponent::player);
        registry.Register<AnimatorComponent>("animator", "Animation")
            .Asset("animation", "Animation", &AnimatorComponent::animation, AssetType::Animation)
            .Asset("idleAnimation", "Au repos", &AnimatorComponent::idleAnimation, AssetType::Animation)
            .Asset("runAnimation", "En courant", &AnimatorComponent::runAnimation, AssetType::Animation)
            .Asset("airAnimation", "En l'air", &AnimatorComponent::airAnimation, AssetType::Animation)
            .Asset("sitDownAnimation", "S'asseoir", &AnimatorComponent::sitDownAnimation, AssetType::Animation)
            .Asset("seatedAnimation", "Assis", &AnimatorComponent::seatedAnimation, AssetType::Animation)
            .Asset("standUpAnimation", "Se lever", &AnimatorComponent::standUpAnimation, AssetType::Animation)
            .Field("speed", "Vitesse", &AnimatorComponent::speed,
                   {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("loop", "En boucle", &AnimatorComponent::loop);
    }
}
