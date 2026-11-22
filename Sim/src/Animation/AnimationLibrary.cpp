#include "Flux/Animation/AnimationLibrary.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <utility>
#include <vector>

namespace Flux {
    AnimationLibrary::AnimationLibrary() = default;
    AnimationLibrary::~AnimationLibrary() = default;

    void AnimationLibrary::SetAssetsDirectory(const std::filesystem::path& directory) {
        m_directory = directory;
        Clear();
    }

    void AnimationLibrary::Clear() {
        m_skeletons.clear();
        m_graphs.clear();
    }

    const ModelSkeleton* AnimationLibrary::GetSkeleton(std::string_view model) {
        if (model.empty()) {
            return nullptr;
        }
        if (const auto found = m_skeletons.find(model); found != m_skeletons.end()) {
            return found->second.get();
        }
        std::unique_ptr<ModelSkeleton>& entry = m_skeletons[std::string(model)];
        // Sans les images : seuls la hiérarchie, les os et les courbes servent ici.
        std::expected<ModelData, std::string> loaded = LoadModelFile(m_directory / Platform::PathFromUtf8(model), false);
        if (!loaded) {
            Log::Warning("Animation : {} : {}", model, loaded.error());
        } else if (!loaded->skeleton.nodes.empty()) {
            entry = std::make_unique<ModelSkeleton>(std::move(loaded->skeleton));
        }
        return entry.get();
    }

    const AnimationGraph* AnimationLibrary::GetGraph(std::string_view graph) {
        if (graph.empty()) {
            return nullptr;
        }
        if (const auto found = m_graphs.find(graph); found != m_graphs.end()) {
            return found->second.get();
        }
        std::unique_ptr<AnimationGraph>& entry = m_graphs[std::string(graph)];
        std::vector<std::uint8_t> bytes;
        if (!Platform::ReadFileBytes(m_directory / Platform::PathFromUtf8(graph), bytes)) {
            Log::Warning("Animation : graphe {} introuvable", graph);
            return nullptr;
        }
        std::expected<AnimationGraph, std::string> parsed =
            ParseAnimationGraph(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (!parsed) {
            Log::Warning("Animation : {} : {}", graph, parsed.error());
            return nullptr;
        }
        entry = std::make_unique<AnimationGraph>(std::move(*parsed));
        return entry.get();
    }

    void AnimationLibrary::SetGraph(std::string_view name, AnimationGraph graph) {
        m_graphs[std::string(name)] = std::make_unique<AnimationGraph>(std::move(graph));
    }

    void AnimationLibrary::SetSkeleton(std::string_view name, ModelSkeleton skeleton) {
        m_skeletons[std::string(name)] = std::make_unique<ModelSkeleton>(std::move(skeleton));
    }
}
