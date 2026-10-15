#include "Panels/Hardpoints.hpp"

#include <Flux/Scene/Weapons.hpp>

namespace FluxEditor::Hardpoints {
    using namespace Flux;

    Entity FindNearby(const Scene& scene, Entity weapon) {
        const Math::WorldPosition position = scene.GetWorldPosition(weapon);
        Entity nearest = NullEntity;
        double nearestDistance = SnapDistance;
        // Une poignée d'emports par vaisseau : un parcours de tous, une fois au lâcher, suffit.
        for (const Entity hardpoint : scene.GetRegistry().view<const HardpointComponent>()) {
            const double distance = Math::Length(Math::Delta(position, scene.GetWorldPosition(hardpoint)));
            if (distance < nearestDistance) {
                nearest = hardpoint;
                nearestDistance = distance;
            }
        }
        return nearest;
    }

    std::string GetRefusal(const Scene& scene, Entity weapon, Entity hardpoint) {
        const auto* component = scene.TryGet<WeaponComponent>(weapon);
        const auto* mount = scene.TryGet<HardpointComponent>(hardpoint);
        if (!component || !mount) {
            return {};
        }
        if (std::string refusal = Weapons::GetRefusal(*mount, *component); !refusal.empty()) {
            return refusal;
        }
        for (const Entity other : scene.GetRegistry().view<const WeaponComponent>()) {
            if (other != weapon && scene.GetParent(other) == hardpoint) {
                return "cet emport porte déjà une arme (« " + scene.Get<NameComponent>(other).name + " »)";
            }
        }
        return {};
    }

    bool Mount(Scene& scene, Entity weapon, Entity hardpoint) {
        if (scene.GetParent(weapon) != hardpoint && !scene.SetParent(weapon, hardpoint)) {
            return false;
        }
        TransformComponent& transform = scene.Get<TransformComponent>(weapon);
        transform.position = {};
        transform.rotation = {};
        return true;
    }
}
