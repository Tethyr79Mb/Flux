#include "Flux/Animation/Animator.hpp"

#include "Flux/Animation/Animation.hpp"
#include "Flux/Animation/AnimationGraph.hpp"
#include "Flux/Animation/AnimationLibrary.hpp"
#include "Flux/Scene/Components.hpp"
#include "Flux/Scene/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <span>
#include <tuple>

namespace Flux::Animator {
    namespace {
        using Graph = AnimationGraph;
        static_assert(Graph::MaxParameters == std::tuple_size_v<decltype(AnimatorComponent::parameters)>);
        static_assert(Graph::MaxLayers == AnimatorComponent::MaxLayers);

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

        // Animations d'un fondu : celles de son état, ou celle de l'animateur sans graphe.
        struct FadeClips
        {
            const ModelAnimation* animations[2] = {};
            std::int16_t indices[2] = {-1, -1}; // dans AnimationSet::animations
            float weights[2] = {};
            float phases[2] = {}; // décalages des cycles
            float starts[2] = {}; // morceau joué : de starts à starts + lengths (Graph::State::start, end)
            float lengths[2] = {};
            int count = 0;
            float scale = 1.0f;
            bool loop = true;
        };

        // Animation de ce nom dans set, rangée en slot de clips.
        void Resolve(FadeClips& clips, int slot, const AnimationSet& set, std::string_view name) {
            const int index = set.Find(name);
            clips.indices[slot] = static_cast<std::int16_t>(index);
            clips.animations[slot] = index >= 0 ? set.animations[static_cast<std::size_t>(index)] : nullptr;
            clips.lengths[slot] = clips.animations[slot] ? clips.animations[slot]->duration : 0.0f;
        }

        // Morceau de l'animation du slot gardé par l'état, borné à sa durée.
        void Trim(FadeClips& clips, int slot, const Graph::State& state) {
            const float duration = clips.lengths[slot];
            const float start = std::min(state.start, duration);
            const float end = state.end > 0.0f ? std::min(state.end, duration) : duration;
            clips.starts[slot] = start;
            clips.lengths[slot] = std::max(end - start, 0.0f);
        }

        FadeClips ClipsOf(const AnimatorComponent& animator, const Graph::StateMachine* machine, const AnimationSet& set,
                          const AnimatorFade& fade) {
            FadeClips clips;
            if (!machine || fade.state < 0) {
                Resolve(clips, 0, set, animator.animation);
                clips.weights[0] = 1.0f;
                clips.count = 1;
                clips.loop = animator.loop;
                return clips;
            }
            const Graph::State& state = machine->states[static_cast<std::size_t>(fade.state)];
            const Mix mix = MixOf(state, fade.blend);
            for (int i = 0; i < mix.count; ++i) {
                Resolve(clips, i, set, mix.points[i]->animation);
                Trim(clips, i, state);
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
        float RateOf(const FadeClips& clips, float speed) {
            float duration = 0.0f;
            for (int i = 0; i < clips.count; ++i) {
                duration += clips.weights[i] * clips.lengths[i];
            }
            return duration > 0.0f ? speed * clips.scale / duration : 0.0f;
        }

        bool Finished(const FadeClips& clips, const AnimatorFade& fade) {
            return !clips.loop && fade.phase >= 1.0f;
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
        const Graph::Transition* FindTransition(const Graph::StateMachine& machine, std::span<const float> parameters, int current,
                                                bool finished) {
            for (const Graph::Transition& transition : machine.transitions) {
                if ((transition.from >= 0 && transition.from != current) || transition.to == current || (transition.atEnd && !finished)) {
                    continue;
                }
                if (std::ranges::all_of(transition.conditions, [&](const Graph::Condition& c) { return Holds(c, parameters); })) {
                    return &transition;
                }
            }
            return nullptr;
        }

        float BlendOf(const Graph::StateMachine* machine, const AnimatorComponent& animator, int state) {
            if (!machine || state < 0) {
                return 0.0f;
            }
            const int parameter = machine->states[static_cast<std::size_t>(state)].blendParameter;
            return parameter >= 0 ? animator.parameters[static_cast<std::size_t>(parameter)] : 0.0f;
        }

        // Part voulue d'une couche : 1 pour le corps entier, sinon son paramètre (borné) ou sa valeur.
        float TargetWeight(const AnimatorComponent& animator, const Graph::Layer& layer, std::size_t index) {
            if (index == 0) {
                return 1.0f;
            }
            if (layer.weightParameter >= 0) {
                return std::clamp(animator.parameters[static_cast<std::size_t>(layer.weightParameter)], 0.0f, 1.0f);
            }
            return layer.weight;
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

        // Nouveau fondu en haut de la pile d'une couche, sur les autres en duration secondes. Pile pleine : le plus
        // ancien est lâché (un léger saut, rare : quatre transitions en un quart de seconde).
        void Push(AnimatorLayer& layer, int state, float duration) {
            if (layer.fadeCount == AnimatorLayer::MaxFades) {
                std::shift_left(layer.fades.begin(), layer.fades.end(), 1);
                --layer.fadeCount;
                layer.fades[0].fade = 1.0f;
            }
            AnimatorFade fade{.state = static_cast<std::int16_t>(state)};
            // Revenue à un état qui s'efface encore (repos, marche, repos) : son cycle continue au lieu de repartir.
            for (std::size_t k = layer.fadeCount; k-- > 0;) {
                if (layer.fades[k].state == state) {
                    fade.phase = layer.fades[k].phase;
                    break;
                }
            }
            if (duration > 0.0f && layer.fadeCount > 0) {
                fade.fade = 0.0f;
                fade.fadeRate = 1.0f / duration;
            }
            layer.fades[layer.fadeCount++] = fade;
        }

        // Retire les fondus entièrement cachés par un plus récent arrivé au bout de son fondu.
        void DropHidden(AnimatorLayer& layer) {
            for (std::size_t k = layer.fadeCount; k-- > 1;) {
                if (layer.fades[k].fade >= 1.0f) {
                    std::shift_left(layer.fades.begin(), layer.fades.begin() + layer.fadeCount, static_cast<std::ptrdiff_t>(k));
                    layer.fadeCount = static_cast<std::uint8_t>(layer.fadeCount - k);
                    break;
                }
            }
            layer.fades[0].fade = 1.0f;
        }

        // Os du masque d'une couche, -1 pour tout le corps, NoMask si l'os n'est pas dans ce squelette (couche coupée :
        // un masque mal nommé ne doit pas animer tout le corps).
        constexpr std::int16_t NoMask = -2;

        std::int16_t MaskOf(const Graph::Layer& layer, const ModelSkeleton& skeleton) {
            if (layer.mask.empty()) {
                return -1;
            }
            const int node = FindNode(skeleton, layer.mask);
            return node >= 0 ? static_cast<std::int16_t>(node) : NoMask;
        }

        void Start(AnimatorComponent& animator, const Graph* graph, const AnimationSet& set) {
            animator.parameters.fill(0.0f);
            animator.layerCount = static_cast<std::uint8_t>(graph ? graph->layers.size() : 1);
            if (graph) {
                for (std::size_t p = 0; p < graph->parameters.size(); ++p) {
                    animator.parameters[p] = graph->parameters[p].value;
                }
            }
            for (std::size_t l = 0; l < animator.layerCount; ++l) {
                AnimatorLayer& layer = animator.layers[l];
                layer = {};
                Push(layer, graph ? graph->layers[l].start : -1, 0.0f);
                if (graph) {
                    layer.weight = TargetWeight(animator, graph->layers[l], l);
                    layer.maskNode = l == 0 ? -1 : MaskOf(graph->layers[l], *set.skeleton);
                    layer.modelSpace = graph->layers[l].modelSpace;
                }
            }
            animator.started = true;
        }

        // Graphe changé ou arrivé (fichier modifié, autre graphe choisi) : ses couches et ses états ne sont plus ceux
        // de la lecture en cours.
        bool Matches(const AnimatorComponent& animator, const Graph* graph) {
            if (animator.layerCount != (graph ? graph->layers.size() : 1)) {
                return false;
            }
            for (std::size_t l = 0; l < animator.layerCount; ++l) {
                const AnimatorLayer& layer = animator.layers[l];
                const int stateCount = graph ? static_cast<int>(graph->layers[l].states.size()) : 0;
                if (layer.fadeCount == 0 || std::ranges::any_of(std::span(layer.fades).first(layer.fadeCount), [&](const AnimatorFade& fade) {
                        return fade.state >= stateCount || (graph != nullptr) != (fade.state >= 0);
                    })) {
                    return false;
                }
            }
            return true;
        }

        // Transitions, cycles et fondus d'une couche pour ce pas.
        void AdvanceLayer(AnimatorComponent& animator, AnimatorLayer& layer, const Graph::StateMachine* machine, const AnimationSet& set,
                          bool fresh, float deltaSeconds) {
            if (machine) {
                // Au début, les transitions sont prises d'un coup jusqu'à l'état qui convient (assis dès le départ).
                for (std::size_t tries = fresh ? machine->states.size() : 1; tries > 0; --tries) {
                    AnimatorFade& top = layer.fades[layer.fadeCount - 1];
                    const bool finished = Finished(ClipsOf(animator, machine, set, top), top);
                    const Graph::Transition* transition = FindTransition(*machine, animator.parameters, top.state, finished);
                    if (!transition) {
                        break;
                    }
                    if (fresh) {
                        top = AnimatorFade{.state = static_cast<std::int16_t>(transition->to)};
                    } else {
                        Push(layer, transition->to, transition->duration);
                    }
                }
            }
            // Seul le fondu du dessus suit les paramètres ; ceux qui s'effacent gardent mélange et vitesse.
            AnimatorFade& top = layer.fades[layer.fadeCount - 1];
            top.blend = BlendOf(machine, animator, top.state);
            top.rate = RateOf(ClipsOf(animator, machine, set, top), animator.speed);
            for (std::size_t k = 0; k < layer.fadeCount; ++k) {
                AnimatorFade& fade = layer.fades[k];
                fade.phase += fade.rate * deltaSeconds;
                fade.fade = std::min(1.0f, fade.fade + fade.fadeRate * deltaSeconds);
            }
            DropHidden(layer);
        }

        // Animations pondérées d'une couche : chaque fondu prend fade de ce qui reste sous lui.
        void WriteClips(AnimatorComponent& animator, std::size_t index, const Graph::StateMachine* machine, const AnimationSet& set) {
            const AnimatorLayer& layer = animator.layers[index];
            float remaining = 1.0f;
            for (std::size_t k = layer.fadeCount; k-- > 0;) {
                const AnimatorFade& fade = layer.fades[k];
                const float share = remaining * (k == 0 ? 1.0f : fade.fade);
                remaining -= share;
                const FadeClips clips = ClipsOf(animator, machine, set, fade);
                for (int i = 0; i < clips.count; ++i) {
                    const float weight = share * clips.weights[i];
                    if (!clips.animations[i] || weight < MinWeight || animator.clipCount == AnimatorComponent::MaxClips) {
                        continue;
                    }
                    // Le cycle est ramené ici dans le morceau de l'état : l'échantillonnage n'a plus qu'à borner.
                    float phase = clips.loop ? fade.phase + clips.phases[i] : std::clamp(fade.phase, 0.0f, 1.0f);
                    const bool whole = clips.starts[i] == 0.0f && clips.lengths[i] == clips.animations[i]->duration;
                    if (clips.loop && !whole) {
                        phase -= std::floor(phase);
                    }
                    animator.clips[animator.clipCount++] = {
                        .animation = clips.indices[i],
                        .layer = static_cast<std::uint8_t>(index),
                        .loop = clips.loop && whole,
                        .time = clips.starts[i] + phase * clips.lengths[i],
                        .weight = weight,
                    };
                }
            }
        }

        // Pas d'un animateur dont le squelette est connu.
        void Advance(AnimatorComponent& animator, const Graph* graph, const AnimationSet& set, const CharacterComponent* character,
                     float deltaSeconds) {
            const bool fresh = !animator.started;
            if (fresh || !Matches(animator, graph)) {
                Start(animator, graph, set);
            }
            if (graph && character) {
                SetCharacterParameters(animator, *graph, *character);
            }
            animator.clipCount = 0;
            for (std::size_t l = 0; l < animator.layerCount; ++l) {
                AnimatorLayer& layer = animator.layers[l];
                const Graph::Layer* machine = graph ? &graph->layers[l] : nullptr;
                if (machine) {
                    // Part de la couche : vers la voulue, en fade secondes (sortir l'arme, la ranger).
                    const float target = TargetWeight(animator, *machine, l);
                    const float step = machine->fade > 0.0f ? deltaSeconds / machine->fade : 1.0f;
                    layer.weight = layer.weight < target ? std::min(target, layer.weight + step) : std::max(target, layer.weight - step);
                }
                AdvanceLayer(animator, layer, machine, set, fresh, deltaSeconds);
                if (l == 0 || (layer.weight > 0.0f && layer.maskNode != NoMask)) {
                    WriteClips(animator, l, machine, set);
                }
            }
        }
    }

    void Update(Scene& scene, AnimationLibrary& library, double deltaSeconds) {
        for (const auto [entity, animator] : scene.GetRegistry().view<AnimatorComponent>().each()) {
            const auto* model = scene.TryGet<ModelComponent>(entity);
            const AnimationSet* set = model ? library.GetAnimationSet(model->model, animator.graph) : nullptr;
            if (!set || set->animations.empty()) {
                animator.clipCount = 0; // la pose de repos
                continue;
            }
            Advance(animator, library.GetGraph(animator.graph), *set, scene.TryGet<CharacterComponent>(entity),
                    static_cast<float>(deltaSeconds));
        }
    }

    void Pose(PoseEvaluator& evaluator, const ModelSkeleton& skeleton, const AnimationSet* set, const AnimatorComponent* animator) {
        // Sans set (pas encore lu), les animations du modèle seules : celles des fichiers du graphe viennent après.
        const std::size_t available = set ? set->animations.size() : skeleton.animations.size();
        const auto animationAt = [&](std::size_t index) { return set ? set->animations[index] : &skeleton.animations[index]; };
        if (!animator || animator->clipCount == 0) {
            const ModelAnimation* first = nullptr;
            if (animator && set) {
                const int index = set->Find(animator->animation);
                first = index >= 0 ? set->animations[static_cast<std::size_t>(index)] : nullptr;
            } else if (animator) {
                first = FindAnimation(skeleton, animator->animation);
            }
            evaluator.Evaluate(skeleton, AnimationSample{first, 0.0, false});
            return;
        }
        std::array<AnimationSample, AnimatorComponent::MaxClips> samples;
        std::size_t count = 0;
        for (const AnimationClipWeight& clip : std::span(animator->clips).first(animator->clipCount)) {
            if (clip.animation >= 0 && static_cast<std::size_t>(clip.animation) < available) {
                samples[count++] = {animationAt(static_cast<std::size_t>(clip.animation)), clip.time, clip.loop, clip.weight, clip.layer};
            }
        }
        // Couches par-dessus le corps (haut du corps…) : l'os de leur masque ; un masque introuvable ne joue rien.
        std::array<PoseLayer, AnimatorComponent::MaxLayers - 1> layers;
        std::size_t layerCount = 0;
        for (std::size_t l = 1; l < animator->layerCount; ++l) {
            const AnimatorLayer& layer = animator->layers[l];
            layers[layerCount++] = {layer.maskNode, layer.maskNode >= -1 ? layer.weight : 0.0f, layer.modelSpace};
        }
        evaluator.EvaluateLayers(skeleton, std::span(samples).first(count), std::span(layers).first(layerCount));
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
        const AnimationSet* set = library.GetAnimationSet(model, animator.graph);
        if (!graph || !set) {
            return 0.0f;
        }
        for (const Graph::Layer& layer : graph->layers) {
            const int state = layer.FindTag(tag);
            if (state < 0) {
                continue;
            }
            const Graph::State& found = layer.states[static_cast<std::size_t>(state)];
            const int index = set->Find(found.points.front().animation);
            const ModelAnimation* animation = index >= 0 ? set->animations[static_cast<std::size_t>(index)] : nullptr;
            const float speed = found.speed * animator.speed;
            if (!animation || speed <= 0.0f) {
                return 0.0f;
            }
            const float start = std::min(found.start, animation->duration);
            const float end = found.end > 0.0f ? std::min(found.end, animation->duration) : animation->duration;
            return std::max(end - start, 0.0f) / speed;
        }
        return 0.0f;
    }
}
