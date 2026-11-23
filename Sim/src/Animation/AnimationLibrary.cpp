#include "Flux/Animation/AnimationLibrary.hpp"

#include "Flux/Animation/Animation.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Platform/Filesystem.hpp"

#include <algorithm>
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
        // Nœud de target pour chaque nœud de source, par nom, et profondeur de chaque nœud de source.
        std::vector<int> nodes(source.nodes.size(), -1);
        std::vector<int> depths(source.nodes.size(), 0);
        for (std::size_t i = 0; i < source.nodes.size(); ++i) {
            nodes[i] = FindNode(target, source.nodes[i].name);
            const int parent = source.nodes[i].parent;
            depths[i] = parent >= 0 ? depths[static_cast<std::size_t>(parent)] + 1 : 0; // parents avant enfants
        }
        std::vector<ModelAnimation> animations;
        animations.reserve(source.animations.size());
        for (const ModelAnimation& animation : source.animations) {
            // Les hanches : le nœud recâblé le plus haut dont la place est animée.
            int hips = -1;
            for (const AnimationChannel& channel : animation.channels) {
                if (channel.path == AnimationPath::Translation && channel.node < nodes.size() && nodes[channel.node] >= 0
                    && (hips < 0 || depths[channel.node] < depths[static_cast<std::size_t>(hips)])) {
                    hips = static_cast<int>(channel.node);
                }
            }
            ModelAnimation& copy = animations.emplace_back();
            copy.name = animation.name;
            copy.duration = animation.duration;
            for (const AnimationChannel& channel : animation.channels) {
                if (channel.node >= nodes.size() || nodes[channel.node] < 0 || channel.path == AnimationPath::Scale) {
                    continue;
                }
                const SkeletonNode& from = source.nodes[channel.node];
                const SkeletonNode& to = target.nodes[static_cast<std::size_t>(nodes[channel.node])];
                const bool cubic = channel.interpolation == AnimationInterpolation::CubicSpline;
                if (channel.path == AnimationPath::Rotation) {
                    // Même écart à la pose de repos : repos de target × (repos de source)⁻¹ × rotation. Linéaire en
                    // les quatre nombres : vaut aussi pour les tangentes d'une spline.
                    const Math::Quaternionf offset = to.rotation * from.rotation.Conjugate();
                    AnimationChannel& moved = copy.channels.emplace_back(channel);
                    moved.node = static_cast<std::uint32_t>(nodes[channel.node]);
                    for (std::size_t v = 0; v + 3 < moved.values.size(); v += 4) {
                        const Math::Quaternionf q = offset * Math::Quaternionf{moved.values[v], moved.values[v + 1], moved.values[v + 2], moved.values[v + 3]};
                        moved.values[v] = q.x;
                        moved.values[v + 1] = q.y;
                        moved.values[v + 2] = q.z;
                        moved.values[v + 3] = q.w;
                    }
                    continue;
                }
                const float fromLength = Math::Length(from.translation);
                const float toLength = Math::Length(to.translation);
                const bool sameBone = Math::Length(from.translation - to.translation) <= 0.01f * std::max(toLength, 1.0e-3f);
                if (static_cast<int>(channel.node) != hips && !sameBone) {
                    continue; // l'os garde sa longueur
                }
                // Hanches : écart au repos mis à l'échelle de target (même rapport pour un autre personnage, une autre
                // unité ou une armature à l'échelle 0,01) ; un os identique : tel quel.
                const float ratio = sameBone || fromLength < 1.0e-6f ? 1.0f : toLength / fromLength;
                AnimationChannel& moved = copy.channels.emplace_back(channel);
                moved.node = static_cast<std::uint32_t>(nodes[channel.node]);
                for (std::size_t v = 0; v + 2 < moved.values.size(); v += 3) {
                    const bool tangent = cubic && (v / 3) % 3 != 1; // spline : tangente d'entrée, valeur, tangente de sortie
                    const Math::Vector3f value{moved.values[v], moved.values[v + 1], moved.values[v + 2]};
                    const Math::Vector3f scaled = tangent ? value * ratio : to.translation + (value - from.translation) * ratio;
                    moved.values[v] = scaled.x;
                    moved.values[v + 1] = scaled.y;
                    moved.values[v + 2] = scaled.z;
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
