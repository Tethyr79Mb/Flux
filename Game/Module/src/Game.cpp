#include <Flux/Core/Engine.hpp>
#include <Flux/Core/GameModule.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/UI/DrawList.hpp>

#include <array>
#include <cstddef>

// Scène de démonstration, en attendant le chargement de scènes depuis un fichier.
namespace {
    using namespace Flux;

    // Environ une année-lumière (9,46e15 m) en secteurs de 1 million de km.
    constexpr std::int64_t FarSectorZ = -9'460'730;

    Entity g_system = NullEntity;
    Entity g_planet = NullEntity;

    // Une grille de cubes posée sur un sol, sous une entité racine placée dans le secteur donné.
    void CreateGrid(Scene& scene, std::string_view name, Math::Vector3i64 sector, Math::LinearColor groundColor) {
        constexpr std::array<Math::LinearColor, 4> palette{{
            {0.80f, 0.25f, 0.20f}, {0.25f, 0.60f, 0.30f}, {0.20f, 0.40f, 0.80f}, {0.85f, 0.65f, 0.20f},
        }};

        const Entity grid = scene.CreateEntity(name);
        scene.Add<SectorComponent>(grid, sector);

        const Entity ground = scene.CreateEntity("Sol", grid);
        scene.Get<TransformComponent>(ground) = {{0.0, -0.05, 0.0}, {}, {60.0f, 0.1f, 60.0f}};
        scene.Add<MeshRendererComponent>(ground, MeshType::Cube, groundColor);

        for (int i = -5; i <= 5; ++i) {
            for (int j = -5; j <= 5; ++j) {
                // Décalage pour rester positif : en C++, le reste d'un nombre négatif est négatif.
                const float height = 1.0f + static_cast<float>((i * 7 + j * 3 + 52) % 4);
                const Entity cube = scene.CreateEntity("Cube", grid);
                scene.Get<TransformComponent>(cube) = {{i * 5.0, height * 0.5, j * 5.0}, {}, {1.0f, height, 1.0f}};
                scene.Add<MeshRendererComponent>(cube, MeshType::Cube, palette[static_cast<std::size_t>((i + j + 20) % 4)]);
            }
        }
    }

    // Soleil, planète et lune imbriqués : chaque enfant suit la rotation de son parent.
    void CreateSolarSystem(Scene& scene) {
        g_system = scene.CreateEntity("Système");
        scene.Get<TransformComponent>(g_system).position = {0.0, 14.0, 0.0};

        const Entity sun = scene.CreateEntity("Soleil", g_system);
        scene.Get<TransformComponent>(sun).scale = {2.5f, 2.5f, 2.5f};
        scene.Add<MeshRendererComponent>(sun, MeshType::Cube, Math::LinearColor{1.0f, 0.75f, 0.2f});

        g_planet = scene.CreateEntity("Planète", g_system);
        scene.Get<TransformComponent>(g_planet).position = {9.0, 0.0, 0.0};
        scene.Add<MeshRendererComponent>(g_planet, MeshType::Cube, Math::LinearColor{0.2f, 0.45f, 0.9f});

        const Entity moon = scene.CreateEntity("Lune", g_planet);
        scene.Get<TransformComponent>(moon) = {{2.5, 0.0, 0.0}, {}, {0.4f, 0.4f, 0.4f}};
        scene.Add<MeshRendererComponent>(moon, MeshType::Cube, Math::LinearColor{0.7f, 0.7f, 0.7f});
    }

    void Update(Engine& engine, double deltaSeconds) {
        Scene& scene = engine.GetScene();
        const auto spin = [&](Entity entity, float radiansPerSecond) {
            auto& rotation = scene.Get<TransformComponent>(entity).rotation;
            const float angle = radiansPerSecond * static_cast<float>(deltaSeconds);
            rotation = Math::Normalize(Math::Quaternionf::FromAxisAngle({0, 1, 0}, angle) * rotation);
        };
        spin(g_system, 0.3f); // la planète tourne autour du soleil...
        spin(g_planet, 1.5f); // ...et la lune autour de la planète

        // T : saute d'une grille à l'autre en changeant seulement de secteur (même point de vue).
        Window& window = engine.GetWindow();
        Camera& camera = engine.GetCamera();
        if (window.GetInput().WasKeyPressed(Key::T)) {
            camera.position.sector.z = camera.position.sector.z == 0 ? FarSectorZ : 0;
        }

        UI::DrawList& overlay = engine.GetRenderer().GetOverlay();
        const float bottom = static_cast<float>(window.GetSize().y);
        constexpr std::string_view Hint = "T : sauter entre la grille de l'origine (sol gris) et sa copie à une année-lumière (sol violet)";
        const Math::Vector2f size = overlay.MeasureText(Hint, 16.0f);
        overlay.AddRect({8.0f, bottom - size.y - 24.0f}, {24.0f + size.x, bottom - 8.0f}, {0.0f, 0.0f, 0.0f, 0.6f});
        overlay.AddText({16.0f, bottom - size.y - 16.0f}, Hint, 16.0f, {1.0f, 1.0f, 1.0f, 1.0f});
    }
}

FLUX_GAME_MODULE_EXPORT std::uint32_t FluxGameApiVersion() {
    return Flux::GameModuleApiVersion;
}

FLUX_GAME_MODULE_EXPORT void FluxRegisterGame(Flux::Engine& engine) {
    Flux::Scene& scene = engine.GetScene();
    CreateGrid(scene, "Grille", {}, {0.30f, 0.30f, 0.32f});
    CreateGrid(scene, "Grille lointaine", {0, 0, FarSectorZ}, {0.35f, 0.15f, 0.45f});
    CreateSolarSystem(scene);
    engine.SetGameUpdate(Update);
}

FLUX_GAME_MODULE_EXPORT void FluxUnregisterGame(Flux::Engine& engine) {
    // Rien du jeu ne doit survivre au déchargement de la DLL : ni son code (Update), ni ses entités.
    engine.SetGameUpdate(nullptr);
    engine.GetScene().Clear();
    g_system = Flux::NullEntity;
    g_planet = Flux::NullEntity;
}
