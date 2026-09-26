#include <Flux/Core/Engine.hpp>
#include <Flux/Core/GameModule.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Window.hpp>
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

    // Environ une année-lumière (9,46e15 m) en secteurs de 1 million de km : la grille lointaine de la démo.
    constexpr std::int64_t FarSectorZ = -9'460'730;

    Entity g_system = NullEntity;
    Entity g_planet = NullEntity;
    Entity g_sun = NullEntity;
    std::string g_status; // résultat de la dernière sauvegarde ou du dernier chargement, affiché à l'écran

    // Réglages de l'animation, modifiables depuis le panneau « Démo ».
    bool g_animate = true;
    float g_orbitSpeed = 0.3f; // rad/s : la planète autour du soleil
    float g_moonSpeed = 1.5f;  // rad/s : la lune autour de la planète

    std::filesystem::path ScenePath(const char* fileName) {
        return Platform::GetExecutableDirectory() / "assets" / "scenes" / fileName;
    }

    void LoadScene(Engine& engine, const char* fileName) {
        Scene& scene = engine.GetScene();
        if (auto loaded = SceneSerializer::Load(scene, ScenePath(fileName)); !loaded) {
            g_status = "Erreur : " + loaded.error();
            std::fprintf(stderr, "%s\n", g_status.c_str());
        } else {
            g_status = std::string(fileName) + " chargée (" + std::to_string(scene.GetEntityCount()) + " entités)";
        }
        // Les entités animées sont retrouvées par leur nom : elles changent à chaque chargement.
        g_system = scene.FindByName("Système");
        g_planet = scene.FindByName("Planète");
        g_sun = scene.FindByName("Soleil");
    }

    void SaveScene(Engine& engine, const char* fileName) {
        auto saved = SceneSerializer::Save(engine.GetScene(), ScenePath(fileName));
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
            ui.DragFloat("Orbite (rad/s)", g_orbitSpeed, 0.01f, -10.0, 10.0, 2);
            ui.DragFloat("Lune (rad/s)", g_moonSpeed, 0.01f, -10.0, 10.0, 2);

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

            // Un avant-goût de l'inspecteur : les composants de deux entités, modifiables en direct.
            Scene& scene = engine.GetScene();
            if (g_planet != NullEntity) {
                ui.Separator();
                ui.Text("Planète");
                auto& transform = scene.Get<TransformComponent>(g_planet);
                ui.DragDouble3("Position##planète", transform.position, 0.05);
                ui.DragFloat3("Échelle##planète", transform.scale, 0.01f, 0.01, 100.0);
            }
            if (g_sun != NullEntity) {
                if (auto* renderer = scene.TryGet<MeshRendererComponent>(g_sun)) {
                    ui.Separator();
                    ui.Text("Soleil");
                    Math::Vector3f color{renderer->color.r, renderer->color.g, renderer->color.b};
                    if (ui.DragFloat3("Couleur##soleil", color, 0.005f, 0.0, 1.0)) {
                        renderer->color = {color.x, color.y, color.z, renderer->color.a};
                    }
                }
            }
        }
        ui.EndPanel();
    }

    void Update(Engine& engine, double deltaSeconds) {
        Scene& scene = engine.GetScene();
        const auto spin = [&](Entity entity, float radiansPerSecond) {
            if (entity == NullEntity) {
                return; // absente du fichier chargé
            }
            auto& rotation = scene.Get<TransformComponent>(entity).rotation;
            const float angle = radiansPerSecond * static_cast<float>(deltaSeconds);
            rotation = Math::Normalize(Math::Quaternionf::FromAxisAngle({0, 1, 0}, angle) * rotation);
        };
        if (g_animate) {
            spin(g_system, g_orbitSpeed); // la planète tourne autour du soleil...
            spin(g_planet, g_moonSpeed);  // ...et la lune autour de la planète
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
    LoadScene(engine, "Demo.scene.json");
    engine.SetGameUpdate(Update);
}

FLUX_GAME_MODULE_EXPORT void FluxUnregisterGame(Flux::Engine& engine) {
    // Rien du jeu ne doit survivre au déchargement de la DLL : ni son code (Update), ni ses entités.
    engine.SetGameUpdate(nullptr);
    engine.GetScene().Clear();
    g_system = Flux::NullEntity;
    g_planet = Flux::NullEntity;
    g_sun = Flux::NullEntity;
}
