// Animation squelettique : lecture du squelette et des courbes glTF, poses (interpolations, hiérarchie, fondu),
// choix de l'animation d'un personnage selon ses pas (Animator).
#include "Check.hpp"

#include "Flux/Renderer/Animation.hpp"
#include "Flux/Renderer/Model.hpp"
#include "Flux/Scene/Animator.hpp"
#include "Flux/Scene/Scene.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
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
        // Fondu à moitié entre le repos et la mi-animation : 22,5° et bras à 1,5 m.
        pose.Evaluate(skeleton, {}, {&animation, 0.5, true}, 0.5f);
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

    bool RunAnimatorTests() {
        Scene scene;
        const Entity hero = scene.CreateEntity("Héros");
        scene.Add<CharacterComponent>(hero, CharacterComponent{.walkSpeed = 2.0f, .runSpeed = 5.0f});
        scene.Add<AnimatorComponent>(hero, AnimatorComponent{.animation = "Marche", .idleAnimation = "Repos",
                                                             .runAnimation = "Course", .airAnimation = "Chute"});
        const auto& animator = scene.Get<AnimatorComponent>(hero);
        auto& character = scene.Get<CharacterComponent>(hero);

        // Début du jeu, immobile : le repos, sans fondu.
        Animator::Update(scene, 0.1);
        CHECK(animator.playing == "Repos" && animator.fade == 1.0f);
        // Il marche : fondu du repos vers la marche, jouée à vitesse 1 à sa vitesse de marche.
        character.groundSpeed = 2.0f;
        Animator::Update(scene, 0.1);
        CHECK(animator.playing == "Marche" && animator.previous == "Repos");
        CHECK(std::abs(animator.fade - 0.4f) < 1.0e-5f && std::abs(animator.time - 0.1) < 1.0e-9);
        Animator::Update(scene, 0.1);
        CHECK(std::abs(animator.time - 0.2) < 1.0e-9);
        // Il court (au-delà de 3,5 m/s) : la course, à 4 / 5 de sa vitesse.
        character.groundSpeed = 4.0f;
        Animator::Update(scene, 0.1);
        CHECK(animator.playing == "Course" && animator.previous == "Marche" && std::abs(animator.time - 0.08) < 1.0e-6);
        Animator::Update(scene, 1.0);
        CHECK(animator.fade == 1.0f);
        // En l'air un instant (une marche) : rien ne change ; plus longtemps (saut) : l'animation en l'air.
        character.groundSpeed = 0.0f;
        character.airTime = 0.1f;
        Animator::Update(scene, 0.1);
        CHECK(animator.playing == "Repos");
        character.airTime = 0.3f;
        Animator::Update(scene, 0.1);
        CHECK(animator.playing == "Chute" && animator.previous == "Repos");
        character.airTime = 0.0f;
        Animator::Update(scene, 1.0);
        CHECK(animator.playing == "Repos");
        character.groundSpeed = 4.0f;
        Animator::Update(scene, 1.0);
        CHECK(animator.playing == "Course");
        // Sans animation de repos, la marche se fige à l'arrêt.
        scene.Get<AnimatorComponent>(hero).idleAnimation.clear();
        scene.Get<AnimatorComponent>(hero).runAnimation.clear();
        character.groundSpeed = 3.0f;
        Animator::Update(scene, 0.5);
        const double walking = animator.time;
        character.groundSpeed = 0.0f;
        Animator::Update(scene, 0.5);
        CHECK(animator.playing == "Marche" && animator.time == walking);

        // Objet qui n'est pas un personnage : l'animation tourne à sa vitesse.
        const Entity door = scene.CreateEntity("Porte");
        scene.Add<AnimatorComponent>(door, AnimatorComponent{.speed = 2.0f});
        Animator::Update(scene, 0.25);
        CHECK(scene.Get<AnimatorComponent>(door).time == 0.5 && scene.Get<AnimatorComponent>(door).fade == 1.0f);
        return true;
    }
}

bool RunAnimationTests() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "FluxAnimationTests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    const bool passed = RunLoadingTests(directory) && RunSamplingTests() && RunAnimatorTests();
    std::filesystem::remove_all(directory, error);
    return passed;
}
