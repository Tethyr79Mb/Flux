#include "HandWeapons.hpp"

#include <Flux/Animation/AnimationLibrary.hpp>
#include <Flux/Animation/Animator.hpp>
#include <Flux/Core/Simulation.hpp>
#include <Flux/Scene/ComponentRegistry.hpp>
#include <Flux/Scene/Components.hpp>
#include <Flux/Scene/Projectiles.hpp>
#include <Flux/Scene/Scene.hpp>

#include <algorithm>

namespace Game::HandWeapons {
    namespace {
        using namespace Flux;

        constexpr Math::Vector3f Forward{0.0f, 0.0f, -1.0f};

        // Première arme de poing portée par ce personnage (enfant direct, accroché à son squelette).
        Entity FindWeapon(Scene& scene, Entity character) {
            for (const auto [entity, weapon] : scene.GetRegistry().view<HandWeaponComponent>().each()) {
                if (scene.GetParent(entity) == character) {
                    return entity;
                }
            }
            return NullEntity;
        }

        void Shoot(Simulation& simulation, Entity player, Entity weaponEntity, HandWeaponComponent& weapon, const PlayerCommand& command) {
            const Scene& scene = simulation.GetScene();
            Math::WorldPosition origin = scene.GetWorldPosition(weaponEntity);
            origin += Math::Vector3d(scene.GetWorldRotation(weaponEntity).Rotate(Forward) * std::max(weapon.muzzle, 0.0f));
            // Vers le point visé au centre de la vue (le canon, à côté de la caméra, ne pointe pas exactement dessus).
            Math::WorldPosition aimPoint = command.viewPosition;
            aimPoint += Math::Vector3d(Math::Normalize(command.viewForward) * AimDistance);
            const Math::Vector3d toAim = Math::Delta(origin, aimPoint);
            if (Math::Length(toAim) < 1.0e-3) {
                return;
            }
            const Math::Vector3f direction = Math::Normalize(Math::Vector3f(toAim));
            Projectile projectile;
            projectile.position = origin;
            projectile.velocity = direction * weapon.projectileSpeed;
            projectile.life = std::max(weapon.range, 0.0f) / weapon.projectileSpeed;
            projectile.damage = weapon.damage;
            projectile.color = weapon.color;
            projectile.ship = player; // ne touche pas son tireur
            simulation.GetProjectiles().push_back(projectile);
        }

        void UpdatePlayer(Simulation& simulation, const Actions& actions, Entity player, float deltaSeconds) {
            Scene& scene = simulation.GetScene();
            const auto* character = scene.TryGet<CharacterComponent>(player);
            const Entity weaponEntity = character ? FindWeapon(scene, player) : NullEntity;
            if (weaponEntity == NullEntity) {
                return;
            }
            const PlayerCommand& command = simulation.GetPlayerCommand(player);
            HandWeaponStateComponent& state = scene.GetRegistry().get_or_emplace<HandWeaponStateComponent>(player);
            HandWeaponComponent& weapon = scene.Get<HandWeaponComponent>(weaponEntity);
            if (character->seat != NullEntity) {
                state.drawn = false; // assis ou aux commandes : rangée
            } else if (command.WasPressed(actions.draw)) {
                state.drawn = !state.drawn;
            }
            const bool aiming = state.drawn && command.IsDown(actions.aim);
            weapon.cooldown = std::max(weapon.cooldown - deltaSeconds, 0.0f);
            // Un coup par clic (pistolet), pas plus vite que sa cadence.
            bool fired = false;
            if (state.drawn && command.WasPressed(actions.fire) && weapon.cooldown <= 0.0f && weapon.projectileSpeed > 0.0f) {
                Shoot(simulation, player, weaponEntity, weapon, command);
                weapon.cooldown = weapon.fireRate > 0.0f ? 1.0f / weapon.fireRate : 0.0f;
                fired = true;
            }
            if (auto* model = scene.TryGet<ModelComponent>(weaponEntity)) {
                model->visible = state.drawn;
            }
            if (auto* animator = scene.TryGet<AnimatorComponent>(player)) {
                if (const AnimationGraph* graph = simulation.GetAnimations().GetGraph(animator->graph)) {
                    Animator::SetParameter(*animator, *graph, ArmedParameter, state.drawn ? 1.0f : 0.0f);
                    Animator::SetParameter(*animator, *graph, AimingParameter, aiming ? 1.0f : 0.0f);
                    Animator::SetParameter(*animator, *graph, FireParameter, fired ? 1.0f : 0.0f);
                }
            }
        }
    }

    void Register(ComponentRegistry& registry) {
        registry.Register<HandWeaponComponent>("handWeapon", "Arme de poing")
            .Field("fireRate", "Cadence (coups/s)", &HandWeaponComponent::fireRate, {.speed = 0.05, .min = 0.1, .max = 50.0, .decimals = 2})
            .Field("projectileSpeed", "Vitesse des projectiles (m/s)", &HandWeaponComponent::projectileSpeed,
                   {.speed = 5.0, .min = 10.0, .max = 1.0e5, .decimals = 0})
            .Field("range", "Portée (m)", &HandWeaponComponent::range, {.speed = 1.0, .min = 1.0, .max = 1.0e5, .decimals = 0})
            .Field("damage", "Dégâts", &HandWeaponComponent::damage, {.speed = 0.1, .min = 0.0, .max = 1.0e6, .decimals = 1})
            .Field("muzzle", "Bouche du canon (m)", &HandWeaponComponent::muzzle, {.speed = 0.005, .min = 0.0, .max = 10.0, .decimals = 3})
            .Field("color", "Couleur des traçantes", &HandWeaponComponent::color, {.alpha = false});
    }

    Actions DeclareActions(CommandActions& actions) {
        return {.draw = actions.Declare(DrawAction), .aim = actions.Declare(AimAction), .fire = actions.Declare(FireAction)};
    }

    void Update(Simulation& simulation, const Actions& actions, double deltaSeconds) {
        for (const Entity player : simulation.GetPlayers()) {
            UpdatePlayer(simulation, actions, player, static_cast<float>(deltaSeconds));
        }
    }

    void Reset(Scene& scene) {
        auto& registry = scene.GetRegistry();
        for (const auto [entity, weapon] : registry.view<HandWeaponComponent>().each()) {
            weapon.cooldown = 0.0f;
            if (auto* model = registry.try_get<ModelComponent>(entity)) {
                model->visible = true;
            }
        }
        registry.clear<HandWeaponStateComponent>();
    }
}
