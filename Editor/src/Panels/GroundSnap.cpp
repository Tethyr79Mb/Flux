#include "Panels/GroundSnap.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Scene/Components.hpp>

#include <algorithm>

namespace FluxEditor {
    using namespace Flux;

    namespace {
        bool IsInside(const Scene& scene, Entity entity, Entity root) {
            for (; entity != NullEntity; entity = scene.GetParent(entity)) {
                if (entity == root) {
                    return true;
                }
            }
            return false;
        }
    }

    bool PrepareGround(Engine& engine, Entity entity, const Math::WorldPosition& origin,
                       std::span<const RelativeTransform> transforms, const Math::Vector3d& position, GroundProbe& probe) {
        const Scene& scene = engine.GetScene();
        if (scene.TryGet<PlanetComponent>(entity)) {
            return false;
        }
        probe.parts.clear();
        probe.boxes.clear();
        for (const RelativeTransform& transform : transforms) {
            if (IsInside(scene, transform.entity, entity)) {
                probe.parts.push_back(transform);
            } else if (!scene.TryGet<PlanetComponent>(transform.entity)) {
                if (const std::optional<Math::Box3f> box = engine.GetMeshBounds(transform.entity)) {
                    probe.boxes.push_back({transform, *box});
                }
            }
        }
        const std::optional<Extent> extent =
            ComputeExtent(probe.parts, position, [&engine](Entity part) { return engine.GetMeshBounds(part); });
        if (!extent) {
            return false;
        }
        probe.extent = *extent;
        // Sur une planète, le sol est son relief, que ces boîtes ne connaissent pas : pas de plan y = 0 non plus.
        const auto planets = scene.GetRegistry().view<const PlanetComponent>();
        probe.planeY = planets.begin() == planets.end() ? std::optional(Math::Delta(origin, Math::WorldPosition{}).y)
                                                        : std::nullopt;
        return true;
    }

    std::optional<double> FindGround(const GroundProbe& probe, const Math::Vector3d& position, std::optional<double> from) {
        const double top = position.y + probe.extent.max.y;
        return FindGroundHeight(probe.boxes, position, probe.extent, std::max(top, from.value_or(top)), probe.planeY);
    }
}
