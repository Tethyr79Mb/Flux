#include <Flux/Core/Engine.hpp>
#include <Flux/Core/GameModule.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/SceneSerializer.hpp>
#include <Flux/UI/DrawList.hpp>

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
    std::string g_status; // résultat de la dernière sauvegarde ou du dernier chargement, affiché à l'écran

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
    }

    void SaveScene(Engine& engine, const char* fileName) {
        auto saved = SceneSerializer::Save(engine.GetScene(), ScenePath(fileName));
        g_status = saved ? std::string(fileName) + " sauvegardée" : "Erreur : " + saved.error();
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
        spin(g_system, 0.3f); // la planète tourne autour du soleil...
        spin(g_planet, 1.5f); // ...et la lune autour de la planète

        Window& window = engine.GetWindow();
        const Input& input = window.GetInput();
        if (input.WasKeyPressed(Key::T)) {
            // Même point de vue, un autre secteur.
            Camera& camera = engine.GetCamera();
            camera.position.sector.z = camera.position.sector.z == 0 ? FarSectorZ : 0;
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

        UI::DrawList& overlay = engine.GetRenderer().GetOverlay();
        const float bottom = static_cast<float>(window.GetSize().y);
        const std::string hint = "T : sauter entre l'origine (sol gris) et une année-lumière (sol violet)\n"
                                 "F5 : recharger Demo.scene.json · F6 : sauvegarder l'état actuel · F7 : recharger la sauvegarde\n"
                                 + g_status;
        const Math::Vector2f size = overlay.MeasureText(hint, 16.0f);
        overlay.AddRect({8.0f, bottom - size.y - 24.0f}, {24.0f + size.x, bottom - 8.0f}, {0.0f, 0.0f, 0.0f, 0.6f});
        overlay.AddText({16.0f, bottom - size.y - 16.0f}, hint, 16.0f, {1.0f, 1.0f, 1.0f, 1.0f});
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
}
