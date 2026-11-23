#include "Flux/Animation/AnimationLibrary.hpp"

#include "Flux/Animation/Animation.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>
#include <vector>

namespace Flux {
    namespace {
        // Nom d'un fichier sans son dossier ni son extension (« Animations/Mixamo/Viser.glb » : « Viser »).
        std::string_view FileStem(std::string_view file) {
            file = file.substr(file.find_last_of("/\\") + 1);
            const std::size_t dot = file.rfind('.');
            return dot == std::string_view::npos ? file : file.substr(0, dot);
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
        // Nœud de target pour chaque nœud de source, par nom ; et l'inverse.
        std::vector<int> nodes(source.nodes.size(), -1);
        std::vector<int> sources(target.nodes.size(), -1);
        for (std::size_t i = 0; i < source.nodes.size(); ++i) {
            nodes[i] = FindNode(target, source.nodes[i].name);
            if (nodes[i] >= 0 && sources[static_cast<std::size_t>(nodes[i])] < 0) {
                sources[static_cast<std::size_t>(nodes[i])] = static_cast<int>(i);
            }
        }
        // Les hanches : le nœud recâblé le plus haut dont la place est animée (sinon le plus haut recâblé). Les
        // rotations sont comparées dans le repère de son parent (l'armature) : celui-ci peut différer d'un fichier
        // à l'autre (unité, axes, nœud racine en plus).
        std::vector<int> depths(source.nodes.size(), 0);
        for (std::size_t i = 0; i < source.nodes.size(); ++i) {
            const int parent = source.nodes[i].parent;
            depths[i] = parent >= 0 ? depths[static_cast<std::size_t>(parent)] + 1 : 0; // parents avant enfants
        }
        int hips = -1;
        for (const ModelAnimation& animation : source.animations) {
            for (const AnimationChannel& channel : animation.channels) {
                if (channel.path == AnimationPath::Translation && channel.node < nodes.size() && nodes[channel.node] >= 0
                    && (hips < 0 || depths[channel.node] < depths[static_cast<std::size_t>(hips)])) {
                    hips = static_cast<int>(channel.node);
                }
            }
        }
        for (std::size_t i = 0; i < source.nodes.size() && hips < 0; ++i) {
            hips = nodes[i] >= 0 ? static_cast<int>(i) : -1;
        }
        if (hips < 0) {
            return {};
        }
        const auto isUnder = [](const ModelSkeleton& skeleton, int node, int ancestor) {
            for (; node >= 0; node = skeleton.nodes[static_cast<std::size_t>(node)].parent) {
                if (node == ancestor) {
                    return true;
                }
            }
            return false;
        };
        const int targetHips = nodes[static_cast<std::size_t>(hips)];
        // Rotation de chaque nœud dans le repère du parent des hanches, d'après des rotations locales (0 au-dessus).
        const auto globals = [&isUnder](const ModelSkeleton& skeleton, int top, const auto& local, std::vector<Math::Quaternionf>& out) {
            out.assign(skeleton.nodes.size(), Math::Quaternionf{});
            for (std::size_t i = 0; i < skeleton.nodes.size(); ++i) {
                if (!isUnder(skeleton, static_cast<int>(i), top)) {
                    continue;
                }
                const int parent = skeleton.nodes[i].parent;
                out[i] = static_cast<int>(i) == top || parent < 0 ? local(i) : out[static_cast<std::size_t>(parent)] * local(i);
            }
        };
        std::vector<Math::Quaternionf> sourceRest;
        std::vector<Math::Quaternionf> targetRest;
        globals(source, hips, [&](std::size_t i) { return source.nodes[i].rotation; }, sourceRest);
        globals(target, targetHips, [&](std::size_t i) { return target.nodes[i].rotation; }, targetRest);

        const SkeletonNode& fromHips = source.nodes[static_cast<std::size_t>(hips)];
        const SkeletonNode& toHips = target.nodes[static_cast<std::size_t>(targetHips)];
        const float fromHeight = Math::Length(fromHips.translation);
        const float hipsRatio = fromHeight > 1.0e-6f ? Math::Length(toHips.translation) / fromHeight : 1.0f;

        // Échantillonnée à 30 images par seconde (celles de Mixamo) : chaque os de target prend la même rotation
        // que dans source par rapport à son repos, dans le repère de l'armature (indépendant des axes propres des
        // os, qui changent selon le logiciel d'export) ; les hanches se déplacent à l'échelle de target ; les autres
        // os gardent leur longueur.
        constexpr float Rate = 30.0f;
        PoseEvaluator evaluator;
        std::vector<Math::Quaternionf> sourcePose;
        std::vector<Math::Quaternionf> targetPose;
        std::vector<ModelAnimation> animations;
        animations.reserve(source.animations.size());
        for (const ModelAnimation& animation : source.animations) {
            const std::size_t frames = static_cast<std::size_t>(std::ceil(std::max(animation.duration, 0.0f) * Rate)) + 1;
            ModelAnimation& copy = animations.emplace_back();
            copy.name = animation.name;
            copy.duration = animation.duration;
            const bool movesHips = std::ranges::any_of(animation.channels, [&](const AnimationChannel& channel) {
                return static_cast<int>(channel.node) == hips && channel.path == AnimationPath::Translation;
            });
            // Une courbe par os recâblé sous les hanches (sa rotation dépend de celles de tous ses ancêtres).
            std::vector<int> channelOf(target.nodes.size(), -1);
            for (std::size_t t = 0; t < target.nodes.size(); ++t) {
                if (sources[t] >= 0 && isUnder(target, static_cast<int>(t), targetHips)) {
                    channelOf[t] = static_cast<int>(copy.channels.size());
                    copy.channels.push_back({static_cast<std::uint32_t>(t), AnimationPath::Rotation, AnimationInterpolation::Linear, {}, {}});
                }
            }
            const std::size_t hipsChannel = copy.channels.size(); // si movesHips
            if (movesHips) {
                copy.channels.push_back({static_cast<std::uint32_t>(targetHips), AnimationPath::Translation, AnimationInterpolation::Linear, {}, {}});
            }
            for (AnimationChannel& channel : copy.channels) {
                channel.times.reserve(frames);
                channel.values.reserve(frames * (channel.path == AnimationPath::Rotation ? 4 : 3));
            }
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const float time = std::min(static_cast<float>(frame) / Rate, animation.duration);
                evaluator.Evaluate(source, AnimationSample{&animation, time, false});
                const std::span<const NodePose> local = evaluator.GetLocalPoses();
                globals(source, hips, [&](std::size_t i) { return local[i].rotation; }, sourcePose);
                // Rotations de target dans le repère de l'armature, parents avant enfants : un os sans pareil dans
                // source suit son parent à son repos.
                targetPose.assign(target.nodes.size(), Math::Quaternionf{});
                for (std::size_t t = 0; t < target.nodes.size(); ++t) {
                    if (!isUnder(target, static_cast<int>(t), targetHips)) {
                        continue;
                    }
                    const int parent = target.nodes[t].parent;
                    const Math::Quaternionf above = static_cast<int>(t) == targetHips || parent < 0 ? Math::Quaternionf{}
                                                                                               : targetPose[static_cast<std::size_t>(parent)];
                    if (sources[t] < 0) {
                        targetPose[t] = above * target.nodes[t].rotation;
                        continue;
                    }
                    const auto s = static_cast<std::size_t>(sources[t]);
                    targetPose[t] = Math::Normalize(sourcePose[s] * sourceRest[s].Conjugate() * targetRest[t]);
                    const Math::Quaternionf rotation = Math::Normalize(above.Conjugate() * targetPose[t]);
                    AnimationChannel& channel = copy.channels[static_cast<std::size_t>(channelOf[t])];
                    channel.times.push_back(time);
                    channel.values.insert(channel.values.end(), {rotation.x, rotation.y, rotation.z, rotation.w});
                }
                if (movesHips) {
                    const Math::Vector3f moved = toHips.translation + (local[static_cast<std::size_t>(hips)].translation - fromHips.translation) * hipsRatio;
                    AnimationChannel& channel = copy.channels[hipsChannel];
                    channel.times.push_back(time);
                    channel.values.insert(channel.values.end(), {moved.x, moved.y, moved.z});
                }
            }
            // Pièces qui coulissent (os de même longueur dans les deux : le même squelette) : leurs courbes telles quelles.
            for (const AnimationChannel& channel : animation.channels) {
                if (channel.path != AnimationPath::Translation || static_cast<int>(channel.node) == hips || channel.node >= nodes.size()
                    || nodes[channel.node] < 0) {
                    continue;
                }
                const SkeletonNode& from = source.nodes[channel.node];
                const SkeletonNode& to = target.nodes[static_cast<std::size_t>(nodes[channel.node])];
                if (Math::Length(from.translation - to.translation) <= 0.01f * std::max(Math::Length(to.translation), 1.0e-3f)) {
                    copy.channels.push_back(channel);
                    copy.channels.back().node = static_cast<std::uint32_t>(nodes[channel.node]);
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
                // Seule dans son fichier : nommée comme lui. Les exports de Mixamo s'appellent tous « mixamo.com ».
                if (animations.size() == 1) {
                    animations[0].name = FileStem(file);
                }
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
