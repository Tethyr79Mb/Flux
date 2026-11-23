#include "Flux/Animation/Animation.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace Flux {
    namespace {
        using Math::Quaternionf;
        using Math::Vector3f;
        using Math::Slerp;

        // Valeur k (composantes components) d'une courbe ; CubicSpline : element 0 tangente d'entrée, 1 valeur,
        // 2 tangente de sortie.
        struct Keys
        {
            const AnimationChannel& channel;
            int components;

            const float* At(std::size_t key, int element) const {
                const bool cubic = channel.interpolation == AnimationInterpolation::CubicSpline;
                const std::size_t index = cubic ? key * 3 + static_cast<std::size_t>(element) : key;
                return channel.values.data() + index * static_cast<std::size_t>(components);
            }
        };

        // Échantillonne une courbe à l'instant time dans out (components floats).
        void Sample(const AnimationChannel& channel, int components, float time, float* out) {
            const Keys keys{channel, components};
            const std::vector<float>& times = channel.times;
            const auto copy = [&](const float* value) { std::copy_n(value, components, out); };
            if (time <= times.front() || times.size() == 1) {
                copy(keys.At(0, 1));
                return;
            }
            if (time >= times.back()) {
                copy(keys.At(times.size() - 1, 1));
                return;
            }
            // Première clé strictement après time : la clé d'avant la précède ou l'égale.
            const std::size_t next = static_cast<std::size_t>(std::ranges::upper_bound(times, time) - times.begin());
            const std::size_t previous = next - 1;
            const float span = times[next] - times[previous];
            const float t = span > 0.0f ? (time - times[previous]) / span : 0.0f;
            switch (channel.interpolation) {
                case AnimationInterpolation::Step: copy(keys.At(previous, 1)); return;
                case AnimationInterpolation::Linear: {
                    const float* a = keys.At(previous, 1);
                    const float* b = keys.At(next, 1);
                    if (components == 4) {
                        const Quaternionf q = Slerp({a[0], a[1], a[2], a[3]}, {b[0], b[1], b[2], b[3]}, t);
                        out[0] = q.x;
                        out[1] = q.y;
                        out[2] = q.z;
                        out[3] = q.w;
                    } else {
                        for (int c = 0; c < components; ++c) {
                            out[c] = a[c] + (b[c] - a[c]) * t;
                        }
                    }
                    return;
                }
                case AnimationInterpolation::CubicSpline: {
                    // Hermite (annexe C de glTF 2.0) : les tangentes sont à l'échelle de l'intervalle.
                    const float t2 = t * t;
                    const float t3 = t2 * t;
                    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
                    const float h10 = (t3 - 2.0f * t2 + t) * span;
                    const float h01 = -2.0f * t3 + 3.0f * t2;
                    const float h11 = (t3 - t2) * span;
                    const float* v0 = keys.At(previous, 1);
                    const float* out0 = keys.At(previous, 2);
                    const float* v1 = keys.At(next, 1);
                    const float* in1 = keys.At(next, 0);
                    for (int c = 0; c < components; ++c) {
                        out[c] = h00 * v0[c] + h10 * out0[c] + h01 * v1[c] + h11 * in1[c];
                    }
                    if (components == 4) {
                        const Quaternionf q = Math::Normalize(Quaternionf{out[0], out[1], out[2], out[3]});
                        out[0] = q.x;
                        out[1] = q.y;
                        out[2] = q.z;
                        out[3] = q.w;
                    }
                    return;
                }
            }
        }
    }

    const ModelAnimation* FindAnimation(const ModelSkeleton& skeleton, std::string_view name) {
        if (skeleton.animations.empty()) {
            return nullptr;
        }
        if (name.empty()) {
            return &skeleton.animations.front();
        }
        const auto found = std::ranges::find(skeleton.animations, name, &ModelAnimation::name);
        return found != skeleton.animations.end() ? &*found : nullptr;
    }

    float AnimationTime(const ModelAnimation& animation, double time, bool loop) {
        const double duration = animation.duration;
        if (!(duration > 0.0) || !std::isfinite(time)) {
            return 0.0f;
        }
        if (!loop) {
            return static_cast<float>(std::clamp(time, 0.0, duration));
        }
        double wrapped = std::fmod(time, duration);
        if (wrapped < 0.0) {
            wrapped += duration;
        }
        return static_cast<float>(wrapped);
    }

    Vector3f SkinPosition(std::span<const Math::Matrix4f> jointMatrices, const ModelSkinVertex& skin, const Vector3f& position) {
        Vector3f result;
        for (std::size_t k = 0; k < 4; ++k) {
            if (skin.weights[k] == 0.0f || skin.joints[k] >= jointMatrices.size()) {
                continue;
            }
            const std::array<float, 4> p = jointMatrices[skin.joints[k]].TransformPoint(position);
            result = result + Vector3f{p[0], p[1], p[2]} * skin.weights[k];
        }
        return result;
    }

    // Rotation d'une matrice 3x3 orthonormée (colonnes x, y, z), méthode de Shepperd : on part de la plus
    // grande composante, pour ne jamais diviser par un petit nombre.
    Math::Quaternionf Math::RotationFromAxes(const Vector3f& x, const Vector3f& y, const Vector3f& z) {
        const float trace = x.x + y.y + z.z;
        Math::Quaternionf q;
        if (trace > 0.0f) {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
        } else if (x.x > y.y && x.x > z.z) {
            const float s = std::sqrt(1.0f + x.x - y.y - z.z) * 2.0f;
            q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
        } else if (y.y > z.z) {
            const float s = std::sqrt(1.0f + y.y - x.x - z.z) * 2.0f;
            q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
        } else {
            const float s = std::sqrt(1.0f + z.z - x.x - y.y) * 2.0f;
            q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
        }
        return Math::Normalize(q);
    }

    NodePose DecomposeMatrix(const Math::Matrix4f& matrix) {
        const auto axis = [&](int c) { return Vector3f{matrix.columns[c][0], matrix.columns[c][1], matrix.columns[c][2]}; };
        NodePose pose;
        pose.translation = axis(3);
        pose.scale = {Math::Length(axis(0)), Math::Length(axis(1)), Math::Length(axis(2))};
        if (pose.scale.x > 0.0f && pose.scale.y > 0.0f && pose.scale.z > 0.0f) {
            pose.rotation = Math::RotationFromAxes(axis(0) / pose.scale.x, axis(1) / pose.scale.y, axis(2) / pose.scale.z);
        }
        return pose;
    }

    Math::Matrix4f ComposeMatrix(const NodePose& pose) {
        const Vector3f x = pose.rotation.Rotate(Vector3f{pose.scale.x, 0.0f, 0.0f});
        const Vector3f y = pose.rotation.Rotate(Vector3f{0.0f, pose.scale.y, 0.0f});
        const Vector3f z = pose.rotation.Rotate(Vector3f{0.0f, 0.0f, pose.scale.z});
        Math::Matrix4f m;
        m.columns[0] = {x.x, x.y, x.z, 0.0f};
        m.columns[1] = {y.x, y.y, y.z, 0.0f};
        m.columns[2] = {z.x, z.y, z.z, 0.0f};
        m.columns[3] = {pose.translation.x, pose.translation.y, pose.translation.z, 1.0f};
        return m;
    }

    namespace {
        // Pose de repos, puis les courbes de l'animation (s'il y en a une) à son instant.
        void SamplePose(const ModelSkeleton& skeleton, const AnimationSample& sample, std::vector<NodePose>& poses) {
            const std::size_t nodeCount = skeleton.nodes.size();
            poses.resize(nodeCount);
            for (std::size_t n = 0; n < nodeCount; ++n) {
                const SkeletonNode& node = skeleton.nodes[n];
                poses[n] = {node.translation, node.rotation, node.scale};
            }
            if (!sample.animation) {
                return;
            }
            const float at = AnimationTime(*sample.animation, sample.time, sample.loop);
            for (const AnimationChannel& channel : sample.animation->channels) {
                if (channel.node >= nodeCount || channel.times.empty()) {
                    continue;
                }
                NodePose& pose = poses[channel.node];
                float value[4] = {};
                switch (channel.path) {
                    case AnimationPath::Translation:
                        Sample(channel, 3, at, value);
                        pose.translation = {value[0], value[1], value[2]};
                        break;
                    case AnimationPath::Rotation:
                        Sample(channel, 4, at, value);
                        pose.rotation = {value[0], value[1], value[2], value[3]};
                        break;
                    case AnimationPath::Scale:
                        Sample(channel, 3, at, value);
                        pose.scale = {value[0], value[1], value[2]};
                        break;
                }
            }
        }
    }

    namespace {
        // Mélange des échantillons de la couche layer dans poses (sample : de quoi échantillonner) ; faux si aucun n'a
        // de part, poses étant alors la pose de repos.
        bool BlendLayer(const ModelSkeleton& skeleton, std::span<const AnimationSample> samples, std::uint8_t layer,
                        std::vector<NodePose>& poses, std::vector<NodePose>& sample) {
            const std::size_t nodeCount = skeleton.nodes.size();
            float total = 0.0f;
            std::size_t count = 0;
            const AnimationSample* only = nullptr;
            for (const AnimationSample& candidate : samples) {
                if (candidate.layer == layer && candidate.weight > 0.0f) {
                    total += candidate.weight;
                    only = &candidate;
                    ++count;
                }
            }
            if (count <= 1) {
                SamplePose(skeleton, only ? *only : AnimationSample{}, poses);
                return only != nullptr;
            }
            // Somme pondérée, rotations ramenées dans le même hémisphère que la première (q et -q sont la même
            // rotation), puis normalisées : assez juste entre poses voisines, sans dépendre de l'ordre des parts.
            poses.assign(nodeCount, NodePose{{}, {0.0f, 0.0f, 0.0f, 0.0f}, {}});
            bool first = true;
            for (const AnimationSample& candidate : samples) {
                if (candidate.layer != layer || candidate.weight <= 0.0f) {
                    continue;
                }
                const float weight = candidate.weight / total;
                SamplePose(skeleton, candidate, sample);
                for (std::size_t n = 0; n < nodeCount; ++n) {
                    NodePose& pose = poses[n];
                    const NodePose& add = sample[n];
                    pose.translation = pose.translation + add.translation * weight;
                    pose.scale = pose.scale + add.scale * weight;
                    const float sign = !first && Math::Dot(pose.rotation, add.rotation) < 0.0f ? -weight : weight;
                    const Quaternionf& q = add.rotation;
                    pose.rotation = {pose.rotation.x + q.x * sign, pose.rotation.y + q.y * sign, pose.rotation.z + q.z * sign,
                                     pose.rotation.w + q.w * sign};
                }
                first = false;
            }
            for (NodePose& pose : poses) {
                pose.rotation = Math::Normalize(pose.rotation);
            }
            return true;
        }
    }

    void PoseEvaluator::EvaluateLayers(const ModelSkeleton& skeleton, std::span<const AnimationSample> samples,
                                       std::span<const PoseLayer> layers) {
        const std::size_t nodeCount = skeleton.nodes.size();
        BlendLayer(skeleton, samples, 0, m_poses, m_sample);
        for (std::size_t l = 0; l < layers.size(); ++l) {
            const PoseLayer& layer = layers[l];
            const float weight = std::clamp(layer.weight, 0.0f, 1.0f);
            if (weight <= 0.0f || layer.node >= static_cast<int>(nodeCount)
                || !BlendLayer(skeleton, samples, static_cast<std::uint8_t>(l + 1), m_layer, m_sample)) {
                continue;
            }
            // Os du masque : le nœud et ses descendants (parents avant enfants : un seul passage).
            m_masked.assign(nodeCount, 0);
            for (std::size_t n = 0; n < nodeCount; ++n) {
                const int parent = skeleton.nodes[n].parent;
                m_masked[n] = layer.node < 0 || static_cast<int>(n) == layer.node || (parent >= 0 && m_masked[static_cast<std::size_t>(parent)]);
                if (!m_masked[n]) {
                    continue;
                }
                NodePose& pose = m_poses[n];
                const NodePose& over = m_layer[n];
                pose.translation = pose.translation + (over.translation - pose.translation) * weight;
                pose.scale = pose.scale + (over.scale - pose.scale) * weight;
                pose.rotation = Math::Slerp(pose.rotation, over.rotation, weight);
            }
        }

        // Parents avant enfants : la matrice du parent est toujours prête.
        m_nodeMatrices.resize(nodeCount);
        for (std::size_t n = 0; n < nodeCount; ++n) {
            const Math::Matrix4f local = ComposeMatrix(m_poses[n]);
            const int parent = skeleton.nodes[n].parent;
            m_nodeMatrices[n] = parent >= 0 ? m_nodeMatrices[static_cast<std::size_t>(parent)] * local : local;
        }
        m_jointMatrices.resize(skeleton.joints.size());
        for (std::size_t j = 0; j < skeleton.joints.size(); ++j) {
            const SkeletonJoint& joint = skeleton.joints[j];
            const Math::Matrix4f& node = joint.node < nodeCount ? m_nodeMatrices[joint.node] : Math::Matrix4f::Identity();
            m_jointMatrices[j] = node * joint.inverseBind;
        }
    }
}
