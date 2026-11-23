// Animation squelettique : lecture du squelette et des courbes glTF, poses (interpolations, hiérarchie, fondu),
// graphes d'animation (lecture du fichier, états, transitions, mélange marche et course, fondus interrompus).
#include "Check.hpp"

#include "Flux/Animation/Animation.hpp"
#include "Flux/Animation/AnimationGraph.hpp"
#include "Flux/Animation/AnimationLibrary.hpp"
#include "Flux/Animation/Animator.hpp"
#include "Flux/Animation/Attachments.hpp"
#include "Flux/Assets/Model.hpp"
#include "Flux/Scene/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Flux;
using Math::Vector3f;

namespace {
    bool Near(const Vector3f& a, const Vector3f& b) {
        return std::abs(a.x - b.x) < 1.0e-4f && std::abs(a.y - b.y) < 1.0e-4f && std::abs(a.z - b.z) < 1.0e-4f;
    }

    std::string Base64(const std::vector<std::uint8_t>& data) {
        constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (std::size_t i = 0; i < data.size(); i += 3) {
            const std::uint32_t chunk = (static_cast<std::uint32_t>(data[i]) << 16)
                                      | (i + 1 < data.size() ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0u)
                                      | (i + 2 < data.size() ? static_cast<std::uint32_t>(data[i + 2]) : 0u);
            out += alphabet[(chunk >> 18) & 63];
            out += alphabet[(chunk >> 12) & 63];
            out += i + 1 < data.size() ? alphabet[(chunk >> 6) & 63] : '=';
            out += i + 2 < data.size() ? alphabet[chunk & 63] : '=';
        }
        return out;
    }

    // Deux os : « racine » à l'origine, « bras » un mètre au-dessus. Un triangle suit le bras ; un second, sans
    // squelette, est une pièce rigide posée 5 m à droite. L'animation (1 s) tourne la racine de 90° autour de z
    // (linéaire) et monte le bras de 1 à 3 m (spline cubique à tangentes nulles).
    std::string SkinnedGltf() {
        std::vector<std::uint8_t> bytes;
        const auto floats = [&bytes](std::initializer_list<float> values) {
            for (const float value : values) {
                std::uint8_t raw[4];
                std::memcpy(raw, &value, 4);
                bytes.insert(bytes.end(), raw, raw + 4);
            }
        };
        const float s = std::sin(0.25f * 3.14159265f);
        floats({0, 1, 0, 1, 1, 0, 0, 2, 0});                            // 0 : positions
        floats({0, 0, 1, 0, 0, 1, 0, 0, 1});                            // 36 : normales
        bytes.insert(bytes.end(), {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}); // 72 : os (le bras, indice 1 de la peau)
        floats({1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0});                   // 84 : poids
        floats({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,         // 132 : inverses de liaison
                1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1});
        floats({0, 1});                                                 // 260 : instants
        floats({0, 0, 0, 1, 0, 0, s, s});                               // 268 : rotations
        floats({0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0}); // 300 : spline (entrée, valeur, sortie)
        return R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0, 1, 3]}],
            "nodes": [{"mesh": 0, "skin": 0}, {"name": "racine", "children": [2]},
                      {"name": "bras", "translation": [0, 1, 0]}, {"mesh": 1, "translation": [5, 0, 0]}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "JOINTS_0": 2, "WEIGHTS_0": 3}}]},
                       {"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}}]}],
            "skins": [{"joints": [1, 2], "inverseBindMatrices": 4}],
            "animations": [{"name": "Salut", "channels": [
                {"sampler": 0, "target": {"node": 1, "path": "rotation"}},
                {"sampler": 1, "target": {"node": 2, "path": "translation"}}],
                "samplers": [{"input": 5, "output": 6}, {"input": 5, "output": 7, "interpolation": "CUBICSPLINE"}]}],
            "accessors": [
                {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
                {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
                {"bufferView": 2, "componentType": 5121, "count": 3, "type": "VEC4"},
                {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC4"},
                {"bufferView": 4, "componentType": 5126, "count": 2, "type": "MAT4"},
                {"bufferView": 5, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1]},
                {"bufferView": 6, "componentType": 5126, "count": 2, "type": "VEC4"},
                {"bufferView": 7, "componentType": 5126, "count": 6, "type": "VEC3"}],
            "bufferViews": [
                {"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 36},
                {"buffer": 0, "byteOffset": 72, "byteLength": 12}, {"buffer": 0, "byteOffset": 84, "byteLength": 48},
                {"buffer": 0, "byteOffset": 132, "byteLength": 128}, {"buffer": 0, "byteOffset": 260, "byteLength": 8},
                {"buffer": 0, "byteOffset": 268, "byteLength": 32}, {"buffer": 0, "byteOffset": 300, "byteLength": 72}],
            "buffers": [{"byteLength": )" + std::to_string(bytes.size())
             + R"(, "uri": "data:application/octet-stream;base64,)" + Base64(bytes) + R"("}]})";
    }

    // Position du sommet v du modèle dans la pose calculée.
    Vector3f Skinned(const ModelData& model, const PoseEvaluator& pose, std::size_t v) {
        return SkinPosition(pose.GetJointMatrices(), model.skinVertices[v], model.vertices[v].position);
    }

    bool RunLoadingTests(const std::filesystem::path& directory) {
        const std::string json = SkinnedGltf();
        const std::filesystem::path path = directory / "bras.gltf";
        std::ofstream(path, std::ios::binary).write(json.data(), static_cast<std::streamsize>(json.size()));
        const auto model = LoadModelFile(path);
        if (!model) {
            std::printf("  %s\n", model.error().c_str());
        }
        CHECK(model);
        for (const std::string& warning : model->warnings) {
            std::printf("  %s\n", warning.c_str());
        }
        CHECK(model->warnings.empty());
        CHECK(model->IsAnimated() && model->skinVertices.size() == model->vertices.size() && model->vertices.size() == 6);
        // Nœuds : le maillage, la racine, le bras (enfant après son parent), la pièce rigide.
        const ModelSkeleton& skeleton = model->skeleton;
        CHECK(skeleton.nodes.size() == 4);
        for (std::size_t n = 0; n < skeleton.nodes.size(); ++n) {
            CHECK(skeleton.nodes[n].parent < static_cast<int>(n));
        }
        // Os : les deux de la peau, puis celui de la pièce rigide.
        CHECK(skeleton.joints.size() == 3);
        CHECK(skeleton.nodes[skeleton.joints[1].node].name == "bras");
        CHECK(model->skinVertices[0].joints[0] == 1 && model->skinVertices[0].weights[0] == 1.0f);
        CHECK(model->skinVertices[3].joints[0] == 2 && model->skinVertices[3].weights[0] == 1.0f);
        // Sommets laissés dans le repère de leur maillage.
        CHECK(Near(model->vertices[3].position, {0, 1, 0}));
        // Boîte de la pose de repos : le triangle du bras, et la pièce rigide 5 m à droite.
        CHECK(Near(model->bounds.min, {0, 1, 0}) && Near(model->bounds.max, {6, 2, 0}));

        CHECK(skeleton.animations.size() == 1);
        const ModelAnimation& animation = skeleton.animations[0];
        CHECK(animation.name == "Salut" && animation.duration == 1.0f && animation.channels.size() == 2);
        CHECK(FindAnimation(skeleton, "") == &animation && FindAnimation(skeleton, "Salut") == &animation);
        CHECK(FindAnimation(skeleton, "Course") == nullptr);

        PoseEvaluator pose;
        // Repos : chaque sommet reste où il a été modelé.
        pose.Evaluate(skeleton, {});
        CHECK(Near(Skinned(*model, pose, 0), {0, 1, 0}) && Near(Skinned(*model, pose, 3), {5, 1, 0}));
        // Mi-animation : racine tournée de 45°, bras monté à 2 m (spline), le sommet (0, 1) du bras à 2 m de l'axe.
        const float h = std::sqrt(2.0f);
        pose.Evaluate(skeleton, {&animation, 0.5, true});
        CHECK(Near(Skinned(*model, pose, 0), {-h, h, 0}));
        CHECK(Near(Skinned(*model, pose, 3), {5, 1, 0})); // la pièce rigide n'est pas animée
        // En boucle, 1,5 s revient à 0,5 s ; sans boucle, la dernière image (90°, bras à 3 m).
        pose.Evaluate(skeleton, {&animation, 1.5, true});
        CHECK(Near(Skinned(*model, pose, 0), {-h, h, 0}));
        pose.Evaluate(skeleton, {&animation, 1.5, false});
        CHECK(Near(Skinned(*model, pose, 0), {-3, 0, 0}));
        // Mélange à parts égales du repos et de la mi-animation : 22,5° et bras à 1,5 m.
        const AnimationSample halves[] = {{nullptr, 0.0, true, 0.5f}, {&animation, 0.5, true, 0.5f}};
        pose.EvaluateBlend(skeleton, halves);
        const float angle = 0.125f * 3.14159265f;
        CHECK(Near(Skinned(*model, pose, 0), {-1.5f * std::sin(angle), 1.5f * std::cos(angle), 0}));
        return true;
    }

    bool RunSamplingTests() {
        // Clés en escalier : la valeur saute à l'instant de la clé suivante, pas avant.
        ModelSkeleton skeleton;
        skeleton.nodes.push_back({"cube", -1, {}, {}, {1, 1, 1}});
        ModelAnimation animation{"saut", 1.0f, {}};
        animation.channels.push_back({0, AnimationPath::Translation, AnimationInterpolation::Step, {0, 1}, {0, 0, 0, 5, 0, 0}});
        animation.channels.push_back({0, AnimationPath::Scale, AnimationInterpolation::Linear, {0, 1}, {1, 1, 1, 3, 3, 3}});
        skeleton.animations.push_back(animation);
        PoseEvaluator pose;
        pose.Evaluate(skeleton, {&skeleton.animations[0], 0.99, false});
        CHECK(Near(pose.GetLocalPoses()[0].translation, {0, 0, 0}));
        CHECK(Near(pose.GetLocalPoses()[0].scale, {2.98f, 2.98f, 2.98f}));
        pose.Evaluate(skeleton, {&skeleton.animations[0], 1.0, false});
        CHECK(Near(pose.GetLocalPoses()[0].translation, {5, 0, 0}));
        // Instants avant le début, négatifs en boucle, ou invalides : jamais hors des clés.
        CHECK(AnimationTime(animation, -0.25, true) == 0.75f);
        CHECK(AnimationTime(animation, -0.25, false) == 0.0f);
        CHECK(AnimationTime(animation, std::nan(""), true) == 0.0f);
        const ModelAnimation empty{"vide", 0.0f, {}};
        CHECK(AnimationTime(empty, 3.0, true) == 0.0f);
        // Os sans squelette cohérent (nœud hors limites) : ignoré, pas de plantage.
        skeleton.joints.push_back({7, Math::Matrix4f::Identity()});
        pose.Evaluate(skeleton, {});
        CHECK(pose.GetJointMatrices().size() == 1);
        return true;
    }

    // Squelette d'un personnage : les animations du graphe du jeu, sans courbes (seules leurs durées comptent ici).
    ModelSkeleton HeroSkeleton() {
        ModelSkeleton skeleton;
        skeleton.nodes.push_back({"hanches", -1, {}, {}, {1, 1, 1}});
        for (const auto& [name, duration] : {std::pair{"Repos", 2.0f}, {"Marche", 1.0f}, {"Course", 0.5f}, {"Chute", 1.0f},
                                             {"S'asseoir", 1.5f}, {"Assis", 1.0f}, {"Se lever", 1.2f}}) {
            skeleton.animations.push_back({name, duration, {}});
        }
        return skeleton;
    }

    // Part de l'animation name dans la sortie de l'animateur, et son instant.
    struct Clip
    {
        float weight = 0.0f;
        float time = 0.0f;
    };

    Clip Find(const AnimatorComponent& animator, const ModelSkeleton& skeleton, std::string_view name) {
        Clip clip;
        for (std::size_t c = 0; c < animator.clipCount; ++c) {
            if (skeleton.animations[static_cast<std::size_t>(animator.clips[c].animation)].name == name) {
                clip.weight += animator.clips[c].weight;
                clip.time = animator.clips[c].time;
            }
        }
        return clip;
    }

    float TotalWeight(const AnimatorComponent& animator) {
        float total = 0.0f;
        for (std::size_t c = 0; c < animator.clipCount; ++c) {
            total += animator.clips[c].weight;
        }
        return total;
    }

    bool Close(float a, float b) {
        return std::abs(a - b) < 1.0e-4f;
    }

    bool RunGraphParsingTests() {
        const auto parse = [](std::string_view text) { return ParseAnimationGraph(text); };
        const auto graph = parse(R"({"parameters": {"speed": 0, "armed": true}, "start": "B",
            "states": [{"name": "A", "animation": "a"}, {"name": "B", "blend": "speed", "points": [{"animation": "a", "value": 1},
                       {"animation": "b", "value": 2, "phase": 0.5}]}, {"name": "C", "animation": "c", "loop": false, "tag": "fin"}],
            "transitions": [{"from": "A", "to": "B", "when": ["speed >= 1.5", "armed != 0"], "duration": 0.5},
                            {"to": "C", "when": ["speed<-2"], "atEnd": true}]})");
        if (!graph) {
            std::printf("  %s\n", graph.error().c_str());
        }
        CHECK(graph && graph->layers.size() == 1 && graph->layers[0].states.size() == 3 && graph->layers[0].start == 1 && graph->layers[0].FindTag("fin") == 2 && graph->layers[0].FindState("Z") < 0);
        CHECK(graph->parameters[static_cast<std::size_t>(graph->FindParameter("armed"))].value == 1.0f);
        const AnimationGraph::State& blend = graph->layers[0].states[1];
        CHECK(blend.points.size() == 2 && blend.points[1].phase == 0.5f && blend.blendParameter == graph->FindParameter("speed"));
        const AnimationGraph::Transition& first = graph->layers[0].transitions[0];
        CHECK(first.from == 0 && first.to == 1 && first.duration == 0.5f && first.conditions.size() == 2);
        CHECK(first.conditions[0].compare == AnimationGraph::Compare::GreaterEqual && first.conditions[0].value == 1.5f);
        const AnimationGraph::Transition& second = graph->layers[0].transitions[1];
        CHECK(second.from == -1 && second.atEnd && second.conditions[0].compare == AnimationGraph::Compare::Less
              && second.conditions[0].value == -2.0f);
        // Fautes de frappe dites, pas ignorées.
        CHECK(!parse(R"({"states": []})"));
        CHECK(!parse(R"({"states": [{"name": "A"}]})"));
        CHECK(!parse(R"({"states": [{"name": "A", "animation": "a"}, {"name": "A", "animation": "b"}]})"));
        CHECK(!parse(R"({"states": [{"name": "A", "animation": "a"}], "transitions": [{"to": "B"}]})"));
        CHECK(!parse(R"({"states": [{"name": "A", "animation": "a"}], "transitions": [{"to": "A", "when": ["vitesse > 1"]}]})"));
        CHECK(!parse(R"({"parameters": {"v": 0}, "states": [{"name": "A", "animation": "a"}], "transitions": [{"to": "A", "when": ["v >> 1"]}]})"));
        CHECK(!parse(R"({"parameters": {"v": 0}, "states": [{"name": "A", "blend": "v", "points": [{"animation": "a", "value": 2},
                         {"animation": "b", "value": 1}]}]})"));
        CHECK(!parse(R"({"states": [{"name": "A", "points": [{"animation": "a"}]}]})")); // sans paramètre de mélange
        return true;
    }

    bool RunLayerParsingTests() {
        const auto graph = ParseAnimationGraph(R"({"parameters": {"armed": 0},
            "states": [{"name": "Marche", "animation": "Marche"}],
            "layers": [{"name": "Haut du corps", "mask": "torse", "weight": "armed", "fade": 0.5,
                        "states": [{"name": "Viser", "animation": "Viser"}, {"name": "Tirer", "animation": "Tirer", "loop": false}],
                        "transitions": [{"from": "Tirer", "to": "Viser", "atEnd": true}]},
                       {"name": "Tête", "mask": "tete", "weight": 0.3, "states": [{"name": "Regarder", "animation": "Regarder"}]}]})");
        if (!graph) {
            std::printf("  %s\n", graph.error().c_str());
        }
        CHECK(graph && graph->layers.size() == 3 && graph->layers[0].name == "Corps" && graph->layers[0].mask.empty());
        const AnimationGraph::Layer& upper = graph->layers[1];
        CHECK(upper.name == "Haut du corps" && upper.mask == "torse" && upper.weightParameter == 0 && upper.fade == 0.5f);
        CHECK(upper.states.size() == 2 && upper.transitions[0].from == 1 && upper.transitions[0].to == 0);
        CHECK(graph->layers[2].weightParameter < 0 && Close(graph->layers[2].weight, 0.3f));
        // Une transition ne voit que les états de sa couche ; poids inconnu ; trop de couches.
        CHECK(!ParseAnimationGraph(R"({"states": [{"name": "A", "animation": "a"}],
            "layers": [{"name": "B", "states": [{"name": "B", "animation": "b"}], "transitions": [{"to": "A"}]}]})"));
        CHECK(!ParseAnimationGraph(R"({"states": [{"name": "A", "animation": "a"}],
            "layers": [{"name": "B", "weight": "armed", "states": [{"name": "B", "animation": "b"}]}]})"));
        const std::string layer = R"({"name": "B", "states": [{"name": "B", "animation": "b"}]})";
        CHECK(!ParseAnimationGraph(R"({"states": [{"name": "A", "animation": "a"}], "layers": [)" + layer + "," + layer + "," + layer + ","
                                   + layer + "]}"));
        return true;
    }

    // Hanches à la racine, torse et jambe dessous, bras sous le torse.
    ModelSkeleton LayeredSkeleton() {
        ModelSkeleton skeleton;
        skeleton.nodes.push_back({"hanches", -1, {}, {}, {1, 1, 1}});
        skeleton.nodes.push_back({"torse", 0, {}, {}, {1, 1, 1}});
        skeleton.nodes.push_back({"jambe", 0, {}, {}, {1, 1, 1}});
        skeleton.nodes.push_back({"bras", 1, {}, {}, {1, 1, 1}});
        // Marche : tout à x = 1 ; Viser : tout à x = 5.
        for (const auto& [name, x] : {std::pair{"Marche", 1.0f}, {"Viser", 5.0f}}) {
            ModelAnimation animation{name, 1.0f, {}};
            for (std::uint32_t node = 1; node < 4; ++node) {
                animation.channels.push_back({node, AnimationPath::Translation, AnimationInterpolation::Step, {0}, {x, 0, 0}});
            }
            skeleton.animations.push_back(std::move(animation));
        }
        return skeleton;
    }

    bool RunLayerTests() {
        const ModelSkeleton skeleton = LayeredSkeleton();
        // Pose : la marche pour le corps, la visée sur le torse et ses enfants seulement, à la part de sa couche.
        PoseEvaluator pose;
        const AnimationSample samples[] = {{&skeleton.animations[0], 0.0, true, 1.0f, 0}, {&skeleton.animations[1], 0.0, true, 1.0f, 1}};
        const PoseLayer full[] = {{1, 1.0f}};
        pose.EvaluateLayers(skeleton, samples, full);
        CHECK(Near(pose.GetLocalPoses()[1].translation, {5, 0, 0}) && Near(pose.GetLocalPoses()[3].translation, {5, 0, 0}));
        CHECK(Near(pose.GetLocalPoses()[2].translation, {1, 0, 0}));
        const PoseLayer half[] = {{1, 0.5f}};
        pose.EvaluateLayers(skeleton, samples, half);
        CHECK(Near(pose.GetLocalPoses()[1].translation, {3, 0, 0}) && Near(pose.GetLocalPoses()[2].translation, {1, 0, 0}));
        const PoseLayer whole[] = {{-1, 1.0f}};
        pose.EvaluateLayers(skeleton, samples, whole);
        CHECK(Near(pose.GetLocalPoses()[2].translation, {5, 0, 0}));

        // Couche vue du modèle : le clip tourne les hanches de 54° (tir de profil) ; le torse, masqué, garde cette
        // orientation sur les hanches de face de la marche, au lieu de la perdre avec elles.
        {
            ModelSkeleton turned;
            turned.nodes.push_back({"hanches", -1, {}, {}, {1, 1, 1}});
            turned.nodes.push_back({"torse", 0, {}, {}, {1, 1, 1}});
            const Math::Quaternionf yaw = Math::Quaternionf::FromAxisAngle({0, 1, 0}, 0.94f);
            ModelAnimation profile{"Profil", 1.0f, {}};
            profile.channels.push_back({0, AnimationPath::Rotation, AnimationInterpolation::Step, {0}, {yaw.x, yaw.y, yaw.z, yaw.w}});
            turned.animations.push_back(std::move(profile));
            const AnimationSample over[] = {{nullptr, 0.0, true, 1.0f, 0}, {&turned.animations[0], 0.0, true, 1.0f, 1}};
            const auto same = [](const Math::Quaternionf& a, const Math::Quaternionf& b) { return std::abs(Math::Dot(a, b)) > 0.9999f; };
            const PoseLayer local[] = {{1, 1.0f}};
            pose.EvaluateLayers(turned, over, local);
            CHECK(same(pose.GetLocalPoses()[1].rotation, Math::Quaternionf::Identity()));
            const PoseLayer model[] = {{1, 1.0f, true}};
            pose.EvaluateLayers(turned, over, model);
            CHECK(same(pose.GetLocalPoses()[0].rotation, Math::Quaternionf::Identity()) && same(pose.GetLocalPoses()[1].rotation, yaw));
            CHECK(ParseAnimationGraph(R"({"states": [{"name": "A", "animation": "a"}],
                "layers": [{"name": "B", "mask": "torse", "modelSpace": true, "states": [{"name": "B", "animation": "b"}]}]})")->layers[1].modelSpace);
        }

        // Animateur : la couche du haut du corps suit le paramètre « armed », atteint en 0,5 s.
        AnimationLibrary library;
        auto graph = ParseAnimationGraph(R"({"parameters": {"armed": 0}, "states": [{"name": "Marche", "animation": "Marche"}],
            "layers": [{"name": "Haut du corps", "mask": "torse", "weight": "armed", "fade": 0.5,
                        "states": [{"name": "Viser", "animation": "Viser"}]},
                       {"name": "Fausse", "mask": "queue", "states": [{"name": "Viser", "animation": "Viser"}]}]})");
        CHECK(graph);
        library.SetGraph("arme.fluxanim", std::move(*graph));
        library.SetSkeleton("soldat.glb", LayeredSkeleton());
        Scene scene;
        const Entity soldier = scene.CreateEntity("Soldat");
        scene.Add<ModelComponent>(soldier).model = "soldat.glb";
        scene.Add<AnimatorComponent>(soldier, AnimatorComponent{.graph = "arme.fluxanim"});
        AnimatorComponent& animator = scene.Get<AnimatorComponent>(soldier);
        const auto clipsOf = [&](std::uint8_t layer) {
            int count = 0;
            for (std::size_t c = 0; c < animator.clipCount; ++c) {
                count += animator.clips[c].layer == layer ? 1 : 0;
            }
            return count;
        };
        Animator::Update(scene, library, 0.1);
        CHECK(animator.layerCount == 3 && animator.layers[1].maskNode == 1 && animator.layers[1].weight == 0.0f);
        CHECK(clipsOf(0) == 1 && clipsOf(1) == 0);
        // Os du masque absent du squelette : la couche ne joue rien, plutôt que d'animer tout le corps.
        CHECK(animator.layers[2].maskNode < -1 && clipsOf(2) == 0);
        CHECK(Animator::SetParameter(animator, *library.GetGraph("arme.fluxanim"), "armed", 1.0f));
        Animator::Update(scene, library, 0.25);
        CHECK(Close(animator.layers[1].weight, 0.5f) && clipsOf(1) == 1);
        Animator::Update(scene, library, 1.0);
        CHECK(animator.layers[1].weight == 1.0f);
        // Rangée : la part redescend de même.
        Animator::SetParameter(animator, *library.GetGraph("arme.fluxanim"), "armed", 0.0f);
        Animator::Update(scene, library, 0.25);
        CHECK(Close(animator.layers[1].weight, 0.5f));

        // Morceau d'un clip : de 1 à 2 s d'une animation de 4 s, en boucle ; une fois, il dure 1 s.
        {
            ModelSkeleton longer = LayeredSkeleton();
            longer.animations[1].duration = 4.0f;
            library.SetSkeleton("long.glb", std::move(longer));
            auto ranged = ParseAnimationGraph(R"({"states": [{"name": "Viser", "animation": "Viser", "start": 1, "end": 2},
                {"name": "Une fois", "animation": "Viser", "start": 1, "end": 2, "loop": false, "tag": "once"}]})");
            CHECK(ranged && ranged->layers[0].states[0].start == 1.0f && ranged->layers[0].states[0].end == 2.0f);
            library.SetGraph("morceau.fluxanim", std::move(*ranged));
            const Entity aimer = scene.CreateEntity("Viseur");
            scene.Add<ModelComponent>(aimer).model = "long.glb";
            scene.Add<AnimatorComponent>(aimer, AnimatorComponent{.graph = "morceau.fluxanim"});
            AnimatorComponent& aim = scene.Get<AnimatorComponent>(aimer);
            Animator::Update(scene, library, 0.25);
            CHECK(aim.clipCount == 1 && !aim.clips[0].loop && Close(aim.clips[0].time, 1.25f));
            Animator::Update(scene, library, 1.0);
            CHECK(Close(aim.clips[0].time, 1.25f));
            CHECK(Close(Animator::GetTagDuration(library, aim, "long.glb", "once"), 1.0f));
            CHECK(!ParseAnimationGraph(R"({"states": [{"name": "A", "animation": "a", "start": 2, "end": 1}]})"));
        }
        return true;
    }

    bool RunAttachmentTests() {
        // Décomposition d'une matrice de nœud : ce qui l'a composée.
        const NodePose built{{1, 2, 3}, Math::Quaternionf::FromAxisAngle({0, 1, 0}, 0.7f), {2, 2, 2}};
        const NodePose back = DecomposeMatrix(ComposeMatrix(built));
        CHECK(Near(back.translation, built.translation) && Near(back.scale, built.scale));
        CHECK(std::abs(std::abs(Math::Dot(back.rotation, built.rotation)) - 1.0f) < 1.0e-5f);

        // Main un mètre au-dessus des hanches ; l'animation « Lever » la monte à 2 m, tournée d'un quart de tour (y).
        ModelSkeleton skeleton;
        skeleton.nodes.push_back({"hanches", -1, {}, {}, {1, 1, 1}});
        skeleton.nodes.push_back({"main", 0, {0, 1, 0}, {}, {1, 1, 1}});
        const Math::Quaternionf quarter = Math::Quaternionf::FromAxisAngle({0, 1, 0}, 0.5f * 3.14159265f);
        ModelAnimation raise{"Lever", 1.0f, {}};
        raise.channels.push_back({1, AnimationPath::Translation, AnimationInterpolation::Step, {0}, {0, 2, 0}});
        raise.channels.push_back({1, AnimationPath::Rotation, AnimationInterpolation::Step, {0}, {quarter.x, quarter.y, quarter.z, quarter.w}});
        skeleton.animations.push_back(raise);
        AnimationLibrary library;
        library.SetSkeleton("soldat.glb", skeleton);

        Scene scene;
        const Entity soldier = scene.CreateEntity("Soldat");
        scene.Add<ModelComponent>(soldier).model = "soldat.glb";
        scene.Add<AnimatorComponent>(soldier);
        const Entity gun = scene.CreateEntity("Pistolet", soldier);
        scene.Add<BoneAttachmentComponent>(gun, BoneAttachmentComponent{.bone = "main", .position = {0, 0, 0.1f}});
        const Entity lost = scene.CreateEntity("Perdu", soldier);
        scene.Get<TransformComponent>(lost).position = {7, 0, 0};
        scene.Add<BoneAttachmentComponent>(lost, BoneAttachmentComponent{.bone = "queue"});
        PoseEvaluator evaluator;
        // Éditeur (rien de joué) : la première image de l'animation ; le décalage suit la rotation de l'os (z -> x).
        Attachments::Update(scene, library, evaluator);
        const TransformComponent& placed = scene.Get<TransformComponent>(gun);
        CHECK(Near(Vector3f(placed.position), {0.1f, 2, 0}));
        CHECK(std::abs(std::abs(Math::Dot(placed.rotation, quarter)) - 1.0f) < 1.0e-5f);
        // Pendant le jeu, d'après les animations de l'animateur ; un os inconnu laisse l'objet où il est.
        Animator::Update(scene, library, 0.5);
        Attachments::Update(scene, library, evaluator);
        CHECK(Near(Vector3f(placed.position), {0.1f, 2, 0}));
        CHECK(Near(Vector3f(scene.Get<TransformComponent>(lost).position), {7, 0, 0}));
        return true;
    }

    bool RunAnimationFileTests() {
        // Liste des fichiers d'animations du graphe.
        auto parsed = ParseAnimationGraph(R"({"files": ["Animations/marche.glb"], "states": [{"name": "Marche", "animation": "Marche"}]})");
        CHECK(parsed && parsed->files.size() == 1 && parsed->files[0] == "Animations/marche.glb");
        CHECK(!ParseAnimationGraph(R"({"files": "marche.glb", "states": [{"name": "A", "animation": "a"}]})"));

        // Modèle : racine, hanches, torse, avec sa propre animation « Repos ». Fichier exporté de Mixamo : os préfixés,
        // un os en plus (« queue ») et dans un autre ordre.
        ModelSkeleton model;
        model.nodes.push_back({"Armature", -1, {}, {}, {1, 1, 1}});
        model.nodes.push_back({"Hips", 0, {}, {}, {1, 1, 1}});
        model.nodes.push_back({"Spine", 1, {}, {}, {1, 1, 1}});
        model.animations.push_back({"Repos", 1.0f, {{2, AnimationPath::Translation, AnimationInterpolation::Step, {0}, {0, 1, 0}}}});
        ModelSkeleton file;
        file.nodes.push_back({"mixamorig:Hips", -1, {}, {}, {1, 1, 1}});
        file.nodes.push_back({"mixamorig:Tail", 0, {}, {}, {1, 1, 1}});
        file.nodes.push_back({"mixamorig:Spine", 0, {}, {}, {1, 1, 1}});
        ModelAnimation walk{"Marche", 2.0f, {}};
        walk.channels.push_back({2, AnimationPath::Translation, AnimationInterpolation::Step, {0}, {0, 3, 0}});
        walk.channels.push_back({1, AnimationPath::Translation, AnimationInterpolation::Step, {0}, {9, 9, 9}});
        walk.channels.push_back({0, AnimationPath::Translation, AnimationInterpolation::Step, {0}, {0, 0, 4}});
        file.animations.push_back(walk);
        file.animations.push_back({"Repos", 1.0f, {}}); // même nom que celle du modèle : celle du modèle reste

        // Recâblage par nom : la queue, absente du modèle, est laissée.
        const std::vector<ModelAnimation> moved = RetargetAnimations(file, model);
        CHECK(moved.size() == 2 && moved[0].duration == 2.0f);
        CHECK(std::ranges::none_of(moved[0].channels, [](const AnimationChannel& channel) { return channel.node == 0; })); // l'armature, au-dessus des hanches

        // Autre personnage (Y Bot), dans une autre unité : hanches à 2 au lieu de 1, torse plus long et tourné au repos.
        {
            ModelSkeleton tall;
            tall.nodes.push_back({"mixamorig:Hips", -1, {0, 2, 0}, {}, {1, 1, 1}});
            const Math::Quaternionf rest = Math::Quaternionf::FromAxisAngle({1, 0, 0}, 0.3f);
            tall.nodes.push_back({"mixamorig:Spine", 0, {0, 0.6f, 0}, rest, {1, 1, 1}});
            ModelSkeleton small;
            small.nodes.push_back({"Hips", -1, {0, 1, 0}, {}, {1, 1, 1}});
            small.nodes.push_back({"Spine", 0, {0, 0.3f, 0}, {}, {1, 1, 1}});
            const Math::Quaternionf bend = Math::Quaternionf::FromAxisAngle({0, 0, 1}, 0.5f);
            const Math::Quaternionf posed = rest * bend; // le torse plié de bend par rapport à son repos
            ModelAnimation bow{"Saluer", 1.0f, {}};
            bow.channels.push_back({0, AnimationPath::Translation, AnimationInterpolation::Linear, {0}, {0.4f, 2.2f, 0}});
            bow.channels.push_back({1, AnimationPath::Translation, AnimationInterpolation::Linear, {0}, {0, 0.6f, 0}});
            bow.channels.push_back({1, AnimationPath::Rotation, AnimationInterpolation::Linear, {0}, {posed.x, posed.y, posed.z, posed.w}});
            tall.animations.push_back(bow);
            const std::vector<ModelAnimation> fitted = RetargetAnimations(tall, small);
            CHECK(fitted.size() == 1);
            // Le torse garde sa longueur ; les hanches se déplacent à l'échelle (moitié) ; le torse tourne comme dans
            // le fichier, vu de l'armature (la même rotation dans l'espace, quels que soient les axes propres des os).
            PoseEvaluator fittedPose;
            fittedPose.Evaluate(small, AnimationSample{&fitted[0], 0.0, false});
            CHECK(Near(fittedPose.GetLocalPoses()[0].translation, {0.2f, 1.1f, 0}) && Near(fittedPose.GetLocalPoses()[1].translation, {0, 0.3f, 0}));
            const Math::Quaternionf expected = rest * bend * rest.Conjugate();
            CHECK(std::abs(std::abs(Math::Dot(fittedPose.GetLocalPoses()[1].rotation, expected)) - 1.0f) < 1.0e-4f);
        }

        AnimationLibrary library;
        library.SetSkeleton("soldat.glb", model);
        library.SetSkeleton("Animations/marche.glb", file);
        library.SetGraph("soldat.fluxanim", std::move(*parsed));
        CHECK(library.FindAnimationSet("soldat.glb", "soldat.fluxanim") == nullptr); // rien lu encore
        const AnimationSet* set = library.GetAnimationSet("soldat.glb", "soldat.fluxanim");
        CHECK(set && set->animations.size() == 3 && set->Find("Marche") == 1 && set->Find("Repos") == 0 && set->Find("") == 0);
        CHECK(library.FindAnimationSet("soldat.glb", "soldat.fluxanim") == set);
        CHECK(library.GetAnimationSet("soldat.glb", "")->animations.size() == 1);
        // Seule dans son fichier (export Mixamo, toujours nommée « mixamo.com ») : le nom du fichier.
        ModelSkeleton single = file;
        single.animations = {{"mixamo.com", 1.0f, {}}};
        library.SetSkeleton("Animations/Mixamo/Viser.glb", single);
        auto aimGraph = ParseAnimationGraph(R"({"files": ["Animations/Mixamo/Viser.glb"], "states": [{"name": "V", "animation": "Viser"}]})");
        CHECK(aimGraph);
        library.SetGraph("viser.fluxanim", std::move(*aimGraph));
        set = library.GetAnimationSet("soldat.glb", "soldat.fluxanim"); // SetGraph a oublié les listes
        const AnimationSet* aimSet = library.GetAnimationSet("soldat.glb", "viser.fluxanim");
        CHECK(aimSet && aimSet->Find("Viser") == 1 && aimSet->Find("mixamo.com") < 0);
        // Noms tapés à la main : sans le préfixe du logiciel.
        CHECK(FindNode(file, "Spine") == 2 && FindNode(file, "mixamorig:Spine") == 2 && FindNode(model, "mixamorig:Hips") == 1);
        CHECK(FindNode(model, "Queue") < 0);

        // L'animateur joue la marche du fichier : son clip est l'indice dans la liste, au-delà de celles du modèle.
        Scene scene;
        const Entity soldier = scene.CreateEntity("Soldat");
        scene.Add<ModelComponent>(soldier).model = "soldat.glb";
        scene.Add<AnimatorComponent>(soldier, AnimatorComponent{.graph = "soldat.fluxanim"});
        Animator::Update(scene, library, 0.5);
        const AnimatorComponent& animator = scene.Get<AnimatorComponent>(soldier);
        CHECK(animator.clipCount == 1 && animator.clips[0].animation == 1 && Close(animator.clips[0].time, 0.5f));
        PoseEvaluator pose;
        Animator::Pose(pose, model, set, &animator);
        CHECK(Near(pose.GetLocalPoses()[2].translation, {0, 3, 0}) && Near(pose.GetLocalPoses()[1].translation, {0, 0, 4}));
        // Sans la liste (rendu avant la première lecture) : l'animation inconnue est laissée, la pose de repos.
        Animator::Pose(pose, model, nullptr, &animator);
        CHECK(Near(pose.GetLocalPoses()[2].translation, {0, 0, 0}));
        return true;
    }

    bool RunAnimatorTests() {
        // Le graphe du personnage du jeu, tel qu'il est livré.
        AnimationLibrary library;
        library.SetAssetsDirectory(FLUX_GAME_ASSETS);
        const AnimationGraph* graph = library.GetGraph(DefaultCharacterGraph);
        CHECK(graph != nullptr);
        library.SetSkeleton("hero.glb", HeroSkeleton());
        const ModelSkeleton& skeleton = *library.GetSkeleton("hero.glb");

        Scene scene;
        const Entity hero = scene.CreateEntity("Héros");
        scene.Add<ModelComponent>(hero).model = "hero.glb";
        scene.Add<CharacterComponent>(hero, CharacterComponent{.walkSpeed = 2.0f, .runSpeed = 5.0f});
        scene.Add<AnimatorComponent>(hero, AnimatorComponent{.graph = std::string(DefaultCharacterGraph)});
        const auto& animator = scene.Get<AnimatorComponent>(hero);
        auto& character = scene.Get<CharacterComponent>(hero);

        // Début du jeu, immobile : le repos, sans fondu.
        Animator::Update(scene, library, 0.1);
        CHECK(animator.layers[0].fadeCount == 1 && Close(Find(animator, skeleton, "Repos").weight, 1.0f));
        CHECK(Close(Find(animator, skeleton, "Repos").time, 0.1f));
        // Il marche (allure 1) : fondu du repos vers la marche, jouée à vitesse 1.
        character.groundSpeed = 2.0f;
        Animator::Update(scene, library, 0.1);
        CHECK(animator.layers[0].fadeCount == 2 && Close(Find(animator, skeleton, "Marche").weight, 0.4f));
        CHECK(Close(Find(animator, skeleton, "Repos").weight, 0.6f) && Close(Find(animator, skeleton, "Marche").time, 0.1f));
        // Entre marche et course (allure 1,5) : moitié de chacune, au même point de leur cycle.
        character.groundSpeed = 3.5f;
        Animator::Update(scene, library, 1.0);
        CHECK(animator.layers[0].fadeCount == 1 && Close(Find(animator, skeleton, "Marche").weight, 0.5f)
              && Close(Find(animator, skeleton, "Course").weight, 0.5f));
        const float walkCycle = Find(animator, skeleton, "Marche").time / 1.0f;
        const float runCycle = Find(animator, skeleton, "Course").time / 0.5f;
        CHECK(Close(walkCycle, runCycle));
        // Plus vite que la course (6 m/s) : la course seule, jouée 6 / 5 plus vite.
        character.groundSpeed = 6.0f;
        Animator::Update(scene, library, 0.1);
        const float runBefore = Find(animator, skeleton, "Course").time;
        Animator::Update(scene, library, 0.1);
        CHECK(Close(Find(animator, skeleton, "Course").weight, 1.0f) && Close(Find(animator, skeleton, "Course").time - runBefore, 0.12f));

        // Arrêt puis départ avant la fin du fondu : trois couches, aucune part perdue (pas de saut de pose), et la
        // marche reprend là où son cycle en était.
        character.groundSpeed = 0.0f;
        Animator::Update(scene, library, 0.1);
        CHECK(animator.layers[0].fadeCount == 2);
        character.groundSpeed = 2.0f;
        Animator::Update(scene, library, 0.1);
        CHECK(animator.layers[0].fadeCount == 3 && Close(TotalWeight(animator), 1.0f));
        CHECK(Close(animator.layers[0].fades[2].fade, 0.4f) && Close(animator.layers[0].fades[1].fade, 0.8f));
        Animator::Update(scene, library, 1.0);
        CHECK(animator.layers[0].fadeCount == 1);

        // En l'air un instant (une marche d'escalier) : rien ne change ; plus longtemps (saut) : la chute.
        character.groundSpeed = 0.0f;
        character.airTime = 0.1f;
        Animator::Update(scene, library, 1.0);
        CHECK(Close(Find(animator, skeleton, "Repos").weight, 1.0f));
        character.airTime = 0.3f;
        Animator::Update(scene, library, 1.0);
        CHECK(Close(Find(animator, skeleton, "Chute").weight, 1.0f));
        character.airTime = 0.0f;

        // Assis dès le début du jeu : directement assis, sans fondu depuis le repos.
        const Entity seated = scene.CreateEntity("Pilote");
        scene.Add<ModelComponent>(seated).model = "hero.glb";
        scene.Add<CharacterComponent>(seated).seatState = SeatState::Seated;
        scene.Add<AnimatorComponent>(seated, AnimatorComponent{.graph = std::string(DefaultCharacterGraph)});
        Animator::Update(scene, library, 0.1);
        const auto& pilot = scene.Get<AnimatorComponent>(seated);
        CHECK(pilot.layers[0].fadeCount == 1 && Close(Find(pilot, skeleton, "Assis").weight, 1.0f));
        // Il se lève : jouée une fois, arrêtée sur sa dernière image.
        scene.Get<CharacterComponent>(seated).seatState = SeatState::StandingUp;
        Animator::Update(scene, library, 3.0);
        CHECK(Close(Find(pilot, skeleton, "Se lever").weight, 1.0f) && Close(Find(pilot, skeleton, "Se lever").time, 1.2f));
        // Durée des gestes lue par les sièges : celle de leur animation, à la vitesse de l'animateur.
        CHECK(Close(Animator::GetTagDuration(library, pilot, "hero.glb", SitDownTag), 1.5f));
        scene.Get<AnimatorComponent>(seated).speed = 2.0f;
        CHECK(Close(Animator::GetTagDuration(library, pilot, "hero.glb", StandUpTag), 0.6f));
        CHECK(Animator::GetTagDuration(library, pilot, "inconnu.glb", StandUpTag) == 0.0f);

        // Paramètres posés par le jeu : seulement ceux que le graphe déclare.
        CHECK(Animator::SetParameter(scene.Get<AnimatorComponent>(hero), *graph, "airTime", 0.0f));
        CHECK(!Animator::SetParameter(scene.Get<AnimatorComponent>(hero), *graph, "inconnu", 1.0f));

        // Objet qui n'est pas un personnage, sans graphe : son animation (la première) tourne à sa vitesse.
        const Entity door = scene.CreateEntity("Porte");
        scene.Add<ModelComponent>(door).model = "hero.glb";
        scene.Add<AnimatorComponent>(door, AnimatorComponent{.speed = 2.0f});
        Animator::Update(scene, library, 0.25);
        CHECK(Close(Find(scene.Get<AnimatorComponent>(door), skeleton, "Repos").time, 0.5f));
        // Modèle inconnu : rien à jouer, la pose de repos.
        scene.Get<ModelComponent>(door).model = "absent.glb";
        Animator::Update(scene, library, 0.25);
        CHECK(scene.Get<AnimatorComponent>(door).clipCount == 0);
        return true;
    }
}

bool RunAnimationTests() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "FluxAnimationTests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    const bool passed = RunLoadingTests(directory) && RunSamplingTests() && RunGraphParsingTests() && RunLayerParsingTests() && RunAnimatorTests() && RunLayerTests() && RunAttachmentTests()
                        && RunAnimationFileTests();
    std::filesystem::remove_all(directory, error);
    return passed;
}
