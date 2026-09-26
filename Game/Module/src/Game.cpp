#include <Flux/Core/Engine.hpp>
#include <Flux/Core/GameModule.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Scene/ComponentRegistry.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/SceneSerializer.hpp>
#include <Flux/UI/Context.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

// Démo : la scène vient de assets/scenes/Demo.scene.json (copié à côté de l'exe à chaque build).
namespace {
    using namespace Flux;

    // Premier composant du jeu : l'entité tourne sur elle-même (et emporte ses enfants).
    // Enregistré dans FluxRegisterGame : sauvegardé, chargé et affiché dans l'inspecteur sans autre code.
    struct SpinComponent
    {
        Math::Vector3f axis{0.0f, 1.0f, 0.0f};
        float speed = 1.0f; // rad/s
    };

    // Environ une année-lumière (9,46e15 m) en secteurs de 1 million de km : la grille lointaine de la démo.
    constexpr std::int64_t FarSectorZ = -9'460'730;

    std::string g_status; // résultat de la dernière sauvegarde ou du dernier chargement, affiché à l'écran

    // Réglages de l'animation, modifiables depuis le panneau « Démo ».
    bool g_animate = true;
    float g_timeScale = 1.0f; // multiplie la vitesse de toutes les rotations

    std::filesystem::path ScenePath(const char* fileName) {
        return Platform::GetExecutableDirectory() / "assets" / "scenes" / fileName;
    }

    void LoadScene(Engine& engine, const char* fileName) {
        Scene& scene = engine.GetScene();
        if (auto loaded = SceneSerializer::Load(scene, engine.GetComponents(), ScenePath(fileName)); !loaded) {
            g_status = "Erreur : " + loaded.error();
            std::fprintf(stderr, "%s\n", g_status.c_str());
        } else {
            g_status = std::string(fileName) + " chargée (" + std::to_string(scene.GetEntityCount()) + " entités)";
        }
    }

    void SaveScene(Engine& engine, const char* fileName) {
        auto saved = SceneSerializer::Save(engine.GetScene(), engine.GetComponents(), ScenePath(fileName));
        g_status = saved ? std::string(fileName) + " sauvegardée" : "Erreur : " + saved.error();
    }

    void JumpSector(Engine& engine) {
        // Même point de vue, un autre secteur.
        Camera& camera = engine.GetCamera();
        camera.position.sector.z = camera.position.sector.z == 0 ? FarSectorZ : 0;
    }

    // Démonstration des widgets FluxUI : chaque ligne lit et modifie directement les variables du jeu.
    void DrawDemoPanel(Engine& engine) {
        UI::Context& ui = engine.GetUi();
        // À droite de l'écran au premier affichage, face au panneau « Moteur ».
        const float right = static_cast<float>(engine.GetWindow().GetSize().x);
        if (ui.BeginPanel("Démo", {std::max(8.0f, right - 368.0f), 8.0f}, 360.0f)) {
            ui.Checkbox("Animation", g_animate);
            ui.DragFloat("Vitesse (x)", g_timeScale, 0.01f, -10.0, 10.0, 2);

            ui.Separator();
            if (ui.Button("Recharger (F5)")) {
                LoadScene(engine, "Demo.scene.json");
            }
            ui.SameLine();
            if (ui.Button("Sauver (F6)")) {
                SaveScene(engine, "Sauvegarde.scene.json");
            }
            ui.SameLine();
            if (ui.Button("Charger (F7)")) {
                LoadScene(engine, "Sauvegarde.scene.json");
            }
            if (ui.Button("Sauter d'une année-lumière (T)")) {
                JumpSector(engine);
            }
            ui.TextDisabled(g_status);
        }
        ui.EndPanel();
    }

    void Update(Engine& engine, double deltaSeconds) {
        // Toutes les entités qui ont SpinComponent, quel que soit leur nom : le système solaire de la démo,
        // mais aussi tout cube auquel on ajoute « Rotation continue » dans l'inspecteur.
        if (g_animate) {
            const float time = g_timeScale * static_cast<float>(deltaSeconds);
            engine.GetScene().GetRegistry().view<const SpinComponent, TransformComponent>().each(
                [&](const SpinComponent& spin, TransformComponent& transform) {
                    const Math::Quaternionf step = Math::Quaternionf::FromAxisAngle(spin.axis, spin.speed * time);
                    transform.rotation = Math::Normalize(step * transform.rotation);
                });
        }

        // Raccourcis muets pendant la saisie dans un champ : taper « 1e7 » ne doit rien déclencher.
        const Input& input = engine.GetWindow().GetInput();
        if (!engine.GetUi().WantsKeyboard()) {
            if (input.WasKeyPressed(Key::T)) {
                JumpSector(engine);
            }
            if (input.WasKeyPressed(Key::F5)) {
                LoadScene(engine, "Demo.scene.json");
            }
            if (input.WasKeyPressed(Key::F6)) {
                SaveScene(engine, "Sauvegarde.scene.json");
            }
            if (input.WasKeyPressed(Key::F7)) {
                LoadScene(engine, "Sauvegarde.scene.json");
            }
        }

        DrawDemoPanel(engine);
    }
}

FLUX_GAME_MODULE_EXPORT std::uint32_t FluxGameApiVersion() {
    return Flux::GameModuleApiVersion;
}

FLUX_GAME_MODULE_EXPORT void FluxRegisterGame(Flux::Engine& engine) {
    // Les composants du jeu d'abord : la scène chargée ensuite les utilise.
    engine.GetComponents()
        .Register<SpinComponent>("spin", "Rotation continue")
        .Field("axis", "Axe", &SpinComponent::axis, {.speed = 0.01})
        .Field("speed", "Vitesse (rad/s)", &SpinComponent::speed, {.speed = 0.01, .min = -100.0, .max = 100.0, .decimals = 2});
    LoadScene(engine, "Demo.scene.json");
    engine.SetGameUpdate(Update);
}

FLUX_GAME_MODULE_EXPORT void FluxUnregisterGame(Flux::Engine& engine) {
    // Rien du jeu ne doit survivre au déchargement de la DLL : ni son code (Update), ni ses entités.
    // La scène et les composants enregistrés sont vidés ensuite par le moteur (GameModule::Unregister).
    engine.SetGameUpdate(nullptr);
}
