#include "Flux/Scene/SceneChange.hpp"

#include "Flux/Scene/Scene.hpp"

namespace Flux::SceneChange {
    namespace {
        std::optional<std::string> UpdateEntity(Scene& scene, Entity entity, double deltaSeconds, bool confirmed) {
            if (auto* change = scene.TryGet<SceneChangeComponent>(entity); change && !change->scene.empty()) {
                change->elapsed += static_cast<float>(deltaSeconds);
                const bool timedOut = change->delay > 0.0f && change->elapsed >= change->delay;
                if (timedOut || (change->onConfirm && confirmed)) {
                    return change->scene;
                }
            }
            std::optional<std::string> result;
            scene.ForEachChild(entity, [&](Entity child) {
                if (!result) {
                    result = UpdateEntity(scene, child, deltaSeconds, confirmed);
                }
            });
            return result;
        }
    }

    std::optional<std::string> Update(Scene& scene, double deltaSeconds, bool confirmed) {
        std::optional<std::string> result;
        scene.ForEachRoot([&](Entity root) {
            if (!result) {
                result = UpdateEntity(scene, root, deltaSeconds, confirmed);
            }
        });
        return result;
    }
}
