#include "Flux/Renderer/Animation.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace Flux {
    namespace {
        using Math::Quaternionf;
        using Math::Vector3f;

        float Dot(const Quaternionf& a, const Quaternionf& b) {
            return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        }

        // Interpolation sphérique, par le plus court chemin ; linéaire puis normalisée quand les deux sont presque
        // confondues (le sinus s'annule).
        Quaternionf Slerp(const Quaternionf& a, Quaternionf b, float t) {
            float cosine = Dot(a, b);
            if (cosine < 0.0f) {
                b = {-b.x, -b.y, -b.z, -b.w};
                cosine = -cosine;
            }
            float wa = 1.0f - t;
            float wb = t;
            if (cosine < 0.9995f) {
                const float angle = std::acos(cosine);
                const float sine = std::sin(angle);
                wa = std::sin((1.0f - t) * angle) / sine;
                wb = std::sin(t * angle) / sine;
            }
            return Math::Normalize(Quaternionf{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb,
                                               a.w * wa + b.w * wb});
        }

        Vector3f Lerp(const Vector3f& a, const Vector3f& b, float t) {
            return a + (b - a) * t;
        }

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

    void PoseEvaluator::Evaluate(const ModelSkeleton& skeleton, const AnimationSample& from, const AnimationSample& to,
                                 float weight) {
        const std::size_t nodeCount = skeleton.nodes.size();
        weight = std::clamp(weight, 0.0f, 1.0f);
        SamplePose(skeleton, to, m_poses);
        if (weight < 1.0f) {
            SamplePose(skeleton, from, m_from);
            for (std::size_t n = 0; n < nodeCount; ++n) {
                NodePose& pose = m_poses[n];
                const NodePose& start = m_from[n];
                pose.translation = Lerp(start.translation, pose.translation, weight);
                pose.rotation = Slerp(start.rotation, pose.rotation, weight);
                pose.scale = Lerp(start.scale, pose.scale, weight);
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
