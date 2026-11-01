#include "Panels/Mounts.hpp"

namespace FluxEditor::Mounts {
    using namespace Flux;

    Entity FindNearby(const Scene& scene, const MountRule& rule, Entity part) {
        const Math::WorldPosition position = scene.GetWorldPosition(part);
        Entity nearest = NullEntity;
        double nearestDistance = SnapDistance;
        // Le jeu dit ce qui est support : un parcours de toute la scène, une fois au lâcher d'une pièce, suffit.
        for (const Entity entity : scene.GetRegistry().view<const TransformComponent>()) {
            if (entity == part || !rule.isMount(scene, entity)) {
                continue;
            }
            const double distance = Math::Length(Math::Delta(position, scene.GetWorldPosition(entity)));
            if (distance < nearestDistance) {
                nearest = entity;
                nearestDistance = distance;
            }
        }
        return nearest;
    }

    bool Mount(Scene& scene, Entity part, Entity mount) {
        if (scene.GetParent(part) != mount && !scene.SetParent(part, mount)) {
            return false;
        }
        TransformComponent& transform = scene.Get<TransformComponent>(part);
        transform.position = {};
        transform.rotation = {};
        return true;
    }
}
