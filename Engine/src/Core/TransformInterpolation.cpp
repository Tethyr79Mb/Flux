#include "TransformInterpolation.hpp"

#include "Flux/Scene/Scene.hpp"

#include <utility>

namespace Flux {
    namespace {
        bool Same(const TransformComponent& a, const TransformComponent& b) {
            return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale;
        }

        Math::Vector3i64 SectorOf(const Scene& scene, Entity entity) {
            const auto* sector = scene.TryGet<SectorComponent>(entity);
            return sector ? sector->sector : Math::Vector3i64{};
        }
    }

    void TransformInterpolation::Restore(Scene& scene) {
        for (Moving& moving : m_moving) {
            if (!std::exchange(moving.applied, false) || !scene.IsValid(moving.entity)) {
                continue;
            }
            if (auto* transform = scene.TryGet<TransformComponent>(moving.entity); transform && Same(*transform, moving.drawn)) {
                *transform = moving.after;
            }
        }
    }

    void TransformInterpolation::BeginStep(const Scene& scene) {
        m_before.clear();
        const auto view = scene.GetRegistry().view<const TransformComponent, const HierarchyComponent>();
        for (const auto [entity, transform, hierarchy] : view.each()) {
            m_before.push_back({entity, hierarchy.parent, SectorOf(scene, entity), transform});
        }
    }

    void TransformInterpolation::EndStep(const Scene& scene) {
        m_moving.clear();
        for (const Snapshot& before : m_before) {
            if (!scene.IsValid(before.entity)) {
                continue;
            }
            const auto* transform = scene.TryGet<TransformComponent>(before.entity);
            // Changé de parent (monté à bord) ou de secteur : les deux places ne sont pas dans le même repère, on
            // dessine directement la nouvelle.
            if (!transform || Same(*transform, before.transform) || scene.GetParent(before.entity) != before.parent
                || SectorOf(scene, before.entity) != before.sector) {
                continue;
            }
            m_moving.push_back({.entity = before.entity, .before = before.transform, .after = *transform, .drawn = *transform});
        }
    }

    void TransformInterpolation::Apply(Scene& scene, double alpha) {
        const float t = static_cast<float>(alpha);
        for (Moving& moving : m_moving) {
            if (!scene.IsValid(moving.entity)) {
                continue;
            }
            auto* transform = scene.TryGet<TransformComponent>(moving.entity);
            if (!transform || !Same(*transform, moving.after)) {
                continue; // changé hors simulation depuis le pas : gardé tel quel
            }
            moving.drawn = {
                .position = moving.before.position + (moving.after.position - moving.before.position) * alpha,
                .rotation = Math::Slerp(moving.before.rotation, moving.after.rotation, t),
                .scale = moving.before.scale + (moving.after.scale - moving.before.scale) * t,
            };
            *transform = moving.drawn;
            moving.applied = true;
        }
    }

    void TransformInterpolation::Clear() {
        m_before.clear();
        m_moving.clear();
    }
}
