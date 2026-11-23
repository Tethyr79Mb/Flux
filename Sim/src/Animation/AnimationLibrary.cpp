#include "Flux/Animation/AnimationLibrary.hpp"

#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace Flux {
    namespace {
        // Nom d'os sans l'espace de noms d'un logiciel (« mixamorig:Hips » : « Hips »).
        std::string_view BareName(std::string_view name) {
            const std::size_t colon = name.rfind(':');
            return colon == std::string_view::npos ? name : name.substr(colon + 1);
        }

        // Fichier lu pour ses animations seules ; nullptr s'il ne peut servir (illisible : dit au journal).
        std::unique_ptr<ModelSkeleton> LoadSkeleton(const std::filesystem::path& directory, std::string_view file) {
            // Sans les images : seuls la hiérarchie, les os et les courbes servent ici.
            std::expected<ModelData, std::string> loaded = LoadModelFile(directory / Platform::PathFromUtf8(file), false);
            if (!loaded) {
                Log::Warning("Animation : {} : {}", file, loaded.error());
                return nullptr;
            }
            if (loaded->skeleton.nodes.empty()) {
                return nullptr;
            }
            return std::make_unique<ModelSkeleton>(std::move(loaded->skeleton));
        }
    }

    int AnimationSet::Find(std::string_view name) const {
        if (name.empty()) {
            return animations.empty() ? -1 : 0; // comme FindAnimation : la première
        }
        const auto found = std::ranges::find(animations, name, [](const ModelAnimation* animation) -> std::string_view { return animation->name; });
        return found != animations.end() ? static_cast<int>(found - animations.begin()) : -1;
    }

    std::vector<ModelAnimation> RetargetAnimations(const ModelSkeleton& source, const ModelSkeleton& target) {
        // Nœud de target pour chaque nœud de source : même nom, sinon même nom sans espace de noms.
        std::vector<int> nodes(source.nodes.size(), -1);
        for (std::size_t i = 0; i < source.nodes.size(); ++i) {
            const std::string_view name = source.nodes[i].name;
            auto found = std::ranges::find(target.nodes, name, &SkeletonNode::name);
            if (found == target.nodes.end()) {
                found = std::ranges::find_if(target.nodes, [&](const SkeletonNode& node) { return BareName(node.name) == BareName(name); });
            }
            nodes[i] = found != target.nodes.end() ? static_cast<int>(found - target.nodes.begin()) : -1;
        }
        std::vector<ModelAnimation> animations;
        animations.reserve(source.animations.size());
        for (const ModelAnimation& animation : source.animations) {
            ModelAnimation& copy = animations.emplace_back();
            copy.name = animation.name;
            copy.duration = animation.duration;
            for (const AnimationChannel& channel : animation.channels) {
                if (channel.node < nodes.size() && nodes[channel.node] >= 0) {
                    AnimationChannel& moved = copy.channels.emplace_back(channel);
                    moved.node = static_cast<std::uint32_t>(nodes[channel.node]);
                }
            }
        }
        return animations;
    }

    AnimationLibrary::AnimationLibrary() = default;
    AnimationLibrary::~AnimationLibrary() = default;

    void AnimationLibrary::SetAssetsDirectory(const std::filesystem::path& directory) {
        m_directory = directory;
        Clear();
    }

    void AnimationLibrary::Clear() {
        m_sets.clear(); // avant les squelettes : leurs animations y pointent
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
        entry = LoadSkeleton(m_directory, model);
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

    const AnimationSet* AnimationLibrary::GetAnimationSet(std::string_view model, std::string_view graph) {
        if (const AnimationSet* found = FindAnimationSet(model, graph)) {
            return found;
        }
        const ModelSkeleton* skeleton = GetSkeleton(model);
        if (!skeleton) {
            return nullptr;
        }
        const AnimationGraph* animationGraph = GetGraph(graph);
        auto set = std::make_unique<AnimationSet>();
        set->skeleton = skeleton;
        if (animationGraph) {
            for (const std::string& file : animationGraph->files) {
                // Gardé lui aussi (m_skeletons) : un autre modèle ou graphe peut le reprendre.
                const ModelSkeleton* source = GetSkeleton(file);
                if (!source || source->animations.empty()) {
                    Log::Warning("Animation : {} : aucune animation pour le graphe {}", file, graph);
                    continue;
                }
                std::vector<ModelAnimation> animations = RetargetAnimations(*source, *skeleton);
                std::ranges::move(animations, std::back_inserter(set->borrowed));
            }
        }
        // Pointeurs pris une fois borrowed rempli : il ne bouge plus.
        set->animations.reserve(skeleton->animations.size() + set->borrowed.size());
        for (const ModelAnimation& animation : skeleton->animations) {
            set->animations.push_back(&animation);
        }
        for (const ModelAnimation& animation : set->borrowed) {
            set->animations.push_back(&animation);
        }
        auto models = m_sets.find(model);
        if (models == m_sets.end()) {
            models = m_sets.emplace(std::string(model), Map<AnimationSet>{}).first;
        }
        std::unique_ptr<AnimationSet>& entry = models->second[std::string(graph)];
        entry = std::move(set);
        return entry.get();
    }

    const AnimationSet* AnimationLibrary::FindAnimationSet(std::string_view model, std::string_view graph) const {
        const auto models = m_sets.find(model);
        if (models == m_sets.end()) {
            return nullptr;
        }
        const auto found = models->second.find(graph);
        return found != models->second.end() ? found->second.get() : nullptr;
    }

    void AnimationLibrary::SetGraph(std::string_view name, AnimationGraph graph) {
        m_sets.clear(); // ses fichiers ont pu changer
        m_graphs[std::string(name)] = std::make_unique<AnimationGraph>(std::move(graph));
    }

    void AnimationLibrary::SetSkeleton(std::string_view name, ModelSkeleton skeleton) {
        if (const auto found = m_sets.find(name); found != m_sets.end()) {
            m_sets.erase(found);
        }
        m_skeletons[std::string(name)] = std::make_unique<ModelSkeleton>(std::move(skeleton));
    }
}
