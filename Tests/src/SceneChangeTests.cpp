// Changement de scène automatique (SceneChangeComponent) : délai, confirmation, sauvegarde.
#include "Check.hpp"

#include "Flux/Scene/ComponentRegistry.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneChange.hpp"
#include "Flux/Scene/SceneSerializer.hpp"

#include <string>

using namespace Flux;

bool RunSceneChangeTests() {
    Scene scene;
    const Entity splash = scene.CreateEntity("Splash");
    scene.Add<SceneChangeComponent>(splash, SceneChangeComponent{.scene = "scenes/Demo.scene.json", .delay = 2.0f,
                                                                 .onConfirm = false});

    // Délai : rien avant 2 s, puis la scène suivante.
    CHECK(!SceneChange::Update(scene, 1.5, false));
    CHECK(!SceneChange::Update(scene, 0.4, true)); // confirmation refusée
    const auto afterDelay = SceneChange::Update(scene, 0.2, false);
    CHECK(afterDelay && *afterDelay == "scenes/Demo.scene.json");

    // Sans délai, sur confirmation seulement ; en pause (temps nul), la minuterie n'avance pas.
    auto& change = scene.Get<SceneChangeComponent>(splash);
    change = SceneChangeComponent{.scene = "scenes/Demo.scene.json", .delay = 0.0f, .onConfirm = true};
    CHECK(!SceneChange::Update(scene, 100.0, false));
    CHECK(SceneChange::Update(scene, 0.0, true).has_value());

    // Sans scène : jamais déclenché. Un enfant est trouvé aussi.
    change.scene.clear();
    CHECK(!SceneChange::Update(scene, 10.0, true));
    const Entity child = scene.CreateEntity("Suite", splash);
    scene.Add<SceneChangeComponent>(child, SceneChangeComponent{.scene = "scenes/Menu.scene.json", .delay = 1.0f});
    const auto fromChild = SceneChange::Update(scene, 1.0, false);
    CHECK(fromChild && *fromChild == "scenes/Menu.scene.json");

    // Sauvegarde : scène, délai et confirmation ; le temps écoulé n'est pas enregistré.
    ComponentRegistry components;
    RegisterEngineComponents(components);
    const std::string text = SceneSerializer::SaveToString(scene, components);
    CHECK(text.find("elapsed") == std::string::npos);
    Scene loaded;
    CHECK(SceneSerializer::LoadFromString(loaded, components, text).has_value());
    const auto* copy = loaded.TryGet<SceneChangeComponent>(loaded.FindByName("Suite"));
    CHECK(copy && copy->scene == "scenes/Menu.scene.json" && copy->delay == 1.0f && copy->onConfirm);
    CHECK(copy->elapsed == 0.0f);
    CHECK(components.Find("sceneChange")->FindField("scene")->asset == AssetType::Scene);
    return true;
}
