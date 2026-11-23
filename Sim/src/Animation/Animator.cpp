#include "Flux/Animation/Animator.hpp"

#include "Flux/Animation/Animation.hpp"
#include "Flux/Animation/AnimationGraph.hpp"
#include "Flux/Animation/AnimationLibrary.hpp"
#include "Flux/Scene/Components.hpp"
#include "Flux/Scene/Scene.hpp"

#include <algorithm>
#include <span>
#include <tuple>

namespace Flux::Animator {
    namespace {
        using Graph = AnimationGraph;
        static_assert(Graph::MaxParameters == std::tuple_size_v<decltype(AnimatorComponent::parameters)>);

        constexpr float MinWeight = 1.0e-4f; // part en dessous de laquelle une animation n'est pas échantillonnée

        // Animations d'une couche à un instant : jusqu'à deux points du mélange et leurs parts, et la vitesse de
        // lecture à appliquer hors des points.
        struct Mix
        {
            const Graph::BlendPoint* points[2] = {};
            float weights[2] = {};
            int count = 0;
            float scale = 1.0f;
        };

        Mix MixOf(const Graph::State& state, float blend) {
            Mix mix;
            const std::vector<Graph::BlendPoint>& points = state.points;
            const auto only = [&mix](const Graph::BlendPoint& point, float value) {
                mix.points[0] = &point;
                mix.weights[0] = 1.0f;
                mix.count = 1;
                mix.scale = point.value > 0.0f ? std::max(value, 0.0f) / point.value : 1.0f;
            };
            if (state.blendParameter < 0) {
                mix.points[0] = &points.front();
                mix.weights[0] = 1.0f;
                mix.count = 1;
            } else if (blend <= points.front().value) {
                only(points.front(), blend);
            } else if (blend >= points.back().value) {
                only(points.back(), blend);
            } else {
                const auto next = std::ranges::upper_bound(points, blend, {}, &Graph::BlendPoint::value);
                const Graph::BlendPoint& a = *(next - 1);
                const Graph::BlendPoint& b = *next;
                const float t = (blend - a.value) / (b.value - a.value);
                mix = {.points = {&a, &b}, .weights = {1.0f - t, t}, .count = 2};
            }
            return mix;
        }

        // Animations d'une couche : celles de son état, ou celle de l'animateur sans graphe.
        struct LayerClips
        {
            const ModelAnimation* animations[2] = {};
            float weights[2] = {};
            float phases[2] = {}; // décalages des cycles
            int count = 0;
            float scale = 1.0f;
            bool loop = true;
        };

        LayerClips ClipsOf(const AnimatorComponent& animator, const Graph* graph, const ModelSkeleton& skeleton,
                           const AnimatorLayer& layer) {
            LayerClips clips;
            if (!graph || layer.state < 0) {
                clips.animations[0] = FindAnimation(skeleton, animator.animation);
                clips.weights[0] = 1.0f;
                clips.count = 1;
                clips.loop = animator.loop;
                return clips;
            }
            const Graph::State& state = graph->states[static_cast<std::size_t>(layer.state)];
            const Mix mix = MixOf(state, layer.blend);
            for (int i = 0; i < mix.count; ++i) {
                clips.animations[i] = FindAnimation(skeleton, mix.points[i]->animation);
                clips.weights[i] = mix.weights[i];
                clips.phases[i] = mix.points[i]->phase;
            }
            clips.count = mix.count;
            clips.scale = mix.scale * state.speed;
            clips.loop = state.loop;
            return clips;
        }

        // Cycles par seconde : la durée du cycle mélangé est la moyenne pondérée de celles des animations, qui
        // restent ainsi calées l'une sur l'autre (même phase).
        float RateOf(const LayerClips& clips, float speed) {
            float duration = 0.0f;
            for (int i = 0; i < clips.count; ++i) {
                duration += clips.animations[i] ? clips.weights[i] * clips.animations[i]->duration : 0.0f;
            }
            return duration > 0.0f ? speed * clips.scale / duration : 0.0f;
        }

        bool Finished(const LayerClips& clips, const AnimatorLayer& layer) {
            return !clips.loop && layer.phase >= 1.0f;
        }

        bool Holds(const Graph::Condition& condition, std::span<const float> parameters) {
            const float value = parameters[static_cast<std::size_t>(condition.parameter)];
            switch (condition.compare) {
                case Graph::Compare::Less: return value < condition.value;
                case Graph::Compare::LessEqual: return value <= condition.value;
                case Graph::Compare::Greater: return value > condition.value;
                case Graph::Compare::GreaterEqual: return value >= condition.value;
                case Graph::Compare::Equal: return value == condition.value;
                case Graph::Compare::NotEqual: return value != condition.value;
            }
            return false;
        }

        // Transition à prendre depuis l'état current, nullptr s'il n'y en a pas.
        const Graph::Transition* FindTransition(const Graph& graph, std::span<const float> parameters, int current, bool finished) {
            for (const Graph::Transition& transition : graph.transitions) {
                if ((transition.from >= 0 && transition.from != current) || transition.to == current || (transition.atEnd && !finished)) {
                    continue;
                }
                if (std::ranges::all_of(transition.conditions, [&](const Graph::Condition& c) { return Holds(c, parameters); })) {
                    return &transition;
                }
            }
            return nullptr;
        }

        float BlendOf(const Graph* graph, const AnimatorComponent& animator, int state) {
            if (!graph || state < 0) {
                return 0.0f;
            }
            const int parameter = graph->states[static_cast<std::size_t>(state)].blendParameter;
            return parameter >= 0 ? animator.parameters[static_cast<std::size_t>(parameter)] : 0.0f;
        }

        // Allure (AnimationParameters::Gait) : 1 à la marche, 2 à la course, en proportion entre et au-delà.
        float GaitOf(const CharacterComponent& character) {
            const float speed = character.groundSpeed;
            const float walk = character.walkSpeed;
            const float run = character.runSpeed;
            if (walk <= 0.0f) {
                return 0.0f;
            }
            if (speed <= walk || run <= walk) {
                return speed / walk;
            }
            if (speed <= run) {
                return 1.0f + (speed - walk) / (run - walk);
            }
            return 2.0f * speed / run;
        }

        void SetCharacterParameters(AnimatorComponent& animator, const Graph& graph, const CharacterComponent& character) {
            SetParameter(animator, graph, AnimationParameters::Speed, character.groundSpeed);
            SetParameter(animator, graph, AnimationParameters::Gait, GaitOf(character));
            SetParameter(animator, graph, AnimationParameters::AirTime, character.airTime);
            SetParameter(animator, graph, AnimationParameters::Seat, static_cast<float>(character.seatState));
        }

        // Nouvelle couche en haut de la pile, fondue sur les autres en duration secondes. Pile pleine : la plus
        // ancienne est lâchée (un léger saut, rare : quatre transitions en un quart de seconde).
        void Push(AnimatorComponent& animator, int state, float duration) {
            if (animator.layerCount == AnimatorComponent::MaxLayers) {
                std::shift_left(animator.layers.begin(), animator.layers.end(), 1);
                --animator.layerCount;
                animator.layers[0].fade = 1.0f;
            }
            AnimatorLayer layer{.state = static_cast<std::int16_t>(state)};
            // Revenue à un état qui s'efface encore (repos, marche, repos) : son cycle continue au lieu de repartir.
            for (std::size_t k = animator.layerCount; k-- > 0;) {
                if (animator.layers[k].state == state) {
                    layer.phase = animator.layers[k].phase;
                    break;
                }
            }
            if (duration > 0.0f && animator.layerCount > 0) {
                layer.fade = 0.0f;
                layer.fadeRate = 1.0f / duration;
            }
            animator.layers[animator.layerCount++] = layer;
        }

        // Retire les couches entièrement cachées par une couche plus récente arrivée au bout de son fondu.
        void DropHidden(AnimatorComponent& animator) {
            for (std::size_t k = animator.layerCount; k-- > 1;) {
                if (animator.layers[k].fade >= 1.0f) {
                    std::shift_left(animator.layers.begin(), animator.layers.begin() + animator.layerCount, static_cast<std::ptrdiff_t>(k));
                    animator.layerCount = static_cast<std::uint8_t>(animator.layerCount - k);
                    break;
                }
            }
            animator.layers[0].fade = 1.0f;
        }

        void Start(AnimatorComponent& animator, const Graph* graph) {
            animator.parameters.fill(0.0f);
            animator.layerCount = 0;
            if (graph) {
                for (std::size_t p = 0; p < graph->parameters.size(); ++p) {
                    animator.parameters[p] = graph->parameters[p].value;
                }
            }
            Push(animator, graph ? graph->start : -1, 0.0f);
            animator.started = true;
        }

        // Pas d'un animateur dont le squelette est connu.
        void Advance(AnimatorComponent& animator, const Graph* graph, const ModelSkeleton& skeleton, const CharacterComponent* character,
                     float deltaSeconds) {
            const bool fresh = !animator.started;
            // Graphe changé ou arrivé (fichier modifié, autre graphe choisi) : ses états ne sont plus ceux des couches.
            const int stateCount = graph ? static_cast<int>(graph->states.size()) : 0;
            if (fresh || animator.layerCount == 0
                || std::ranges::any_of(std::span(animator.layers).first(animator.layerCount),
                                       [&](const AnimatorLayer& layer) { return layer.state >= stateCount || (graph != nullptr) != (layer.state >= 0); })) {
                Start(animator, graph);
            }
            if (graph && character) {
                SetCharacterParameters(animator, *graph, *character);
            }
            if (graph) {
                // Au début, les transitions sont prises d'un coup jusqu'à l'état qui convient (assis dès le départ).
                for (std::size_t tries = fresh ? graph->states.size() : 1; tries > 0; --tries) {
                    AnimatorLayer& top = animator.layers[animator.layerCount - 1];
                    const bool finished = Finished(ClipsOf(animator, graph, skeleton, top), top);
                    const Graph::Transition* transition = FindTransition(*graph, animator.parameters, top.state, finished);
                    if (!transition) {
                        break;
                    }
                    if (fresh) {
                        top = AnimatorLayer{.state = static_cast<std::int16_t>(transition->to)};
                    } else {
                        Push(animator, transition->to, transition->duration);
                    }
                }
            }

            // Seule la couche du dessus suit les paramètres ; celles qui s'effacent gardent mélange et vitesse.
            AnimatorLayer& top = animator.layers[animator.layerCount - 1];
            top.blend = BlendOf(graph, animator, top.state);
            top.rate = RateOf(ClipsOf(animator, graph, skeleton, top), animator.speed);
            for (std::size_t k = 0; k < animator.layerCount; ++k) {
                AnimatorLayer& layer = animator.layers[k];
                layer.phase += layer.rate * deltaSeconds;
                layer.fade = std::min(1.0f, layer.fade + layer.fadeRate * deltaSeconds);
            }
            DropHidden(animator);

            // Parts : chaque couche prend fade de ce qui reste sous elle.
            animator.clipCount = 0;
            float remaining = 1.0f;
            for (std::size_t k = animator.layerCount; k-- > 0;) {
                const AnimatorLayer& layer = animator.layers[k];
                const float share = remaining * (k == 0 ? 1.0f : layer.fade);
                remaining -= share;
                const LayerClips clips = ClipsOf(animator, graph, skeleton, layer);
                for (int i = 0; i < clips.count; ++i) {
                    const float weight = share * clips.weights[i];
                    if (!clips.animations[i] || weight < MinWeight || animator.clipCount == AnimatorComponent::MaxClips) {
                        continue;
                    }
                    const ModelAnimation& animation = *clips.animations[i];
                    const float phase = clips.loop ? layer.phase + clips.phases[i] : std::clamp(layer.phase, 0.0f, 1.0f);
                    animator.clips[animator.clipCount++] = {
                        .animation = static_cast<std::int16_t>(&animation - skeleton.animations.data()),
                        .loop = clips.loop,
                        .time = phase * animation.duration,
                        .weight = weight,
                    };
                }
            }
        }
    }

    void Update(Scene& scene, AnimationLibrary& library, double deltaSeconds) {
        for (const auto [entity, animator] : scene.GetRegistry().view<AnimatorComponent>().each()) {
            const auto* model = scene.TryGet<ModelComponent>(entity);
            const ModelSkeleton* skeleton = model ? library.GetSkeleton(model->model) : nullptr;
            if (!skeleton || skeleton->animations.empty()) {
                animator.clipCount = 0; // la pose de repos
                continue;
            }
            Advance(animator, library.GetGraph(animator.graph), *skeleton, scene.TryGet<CharacterComponent>(entity),
                    static_cast<float>(deltaSeconds));
        }
    }

    bool SetParameter(AnimatorComponent& animator, const AnimationGraph& graph, std::string_view name, float value) {
        const int parameter = graph.FindParameter(name);
        if (parameter < 0) {
            return false;
        }
        animator.parameters[static_cast<std::size_t>(parameter)] = value;
        return true;
    }

    float GetTagDuration(AnimationLibrary& library, const AnimatorComponent& animator, std::string_view model, std::string_view tag) {
        const AnimationGraph* graph = library.GetGraph(animator.graph);
        const ModelSkeleton* skeleton = library.GetSkeleton(model);
        const int state = graph ? graph->FindTag(tag) : -1;
        if (state < 0 || !skeleton) {
            return 0.0f;
        }
        const Graph::State& found = graph->states[static_cast<std::size_t>(state)];
        const ModelAnimation* animation = FindAnimation(*skeleton, found.points.front().animation);
        const float speed = found.speed * animator.speed;
        return animation && speed > 0.0f ? animation->duration / speed : 0.0f;
    }
}
