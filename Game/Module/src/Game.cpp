#include <Flux/Core/Engine.hpp>
#include <Flux/Core/GameModule.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Scene/ComponentRegistry.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/UI/Context.hpp>

#include <algorithm>
#include <cstdint>

// Démo : composants et systèmes du jeu. La scène de démarrage est choisie par le projet (Game.fluxproject).
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

    // Réglages de l'animation, modifiables depuis le panneau « Démo ».
    bool g_animate = true;
    float g_timeScale = 1.0f; // multiplie la vitesse de toutes les rotations

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
            if (ui.Button("Sauter d'une année-lumière (T)")) {
                JumpSector(engine);
            }
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
        }

        DrawDemoPanel(engine);
    }
}

FLUX_GAME_MODULE_EXPORT std::uint32_t FluxGameApiVersion() {
    return Flux::GameModuleApiVersion;
}

FLUX_GAME_MODULE_EXPORT void FluxRegisterGame(Flux::Engine& engine) {
    // Les scènes sont chargées ensuite par l'hôte (Game.exe ou l'éditeur) : elles peuvent utiliser ces composants.
    engine.GetComponents()
        .Register<SpinComponent>("spin", "Rotation continue")
        .Field("axis", "Axe", &SpinComponent::axis, {.speed = 0.01})
        .Field("speed", "Vitesse (rad/s)", &SpinComponent::speed, {.speed = 0.01, .min = -100.0, .max = 100.0, .decimals = 2});
    engine.SetGameUpdate(Update);
    // Exemple d'action « Fonction C++ » d'un bouton d'interface (cible : « Exemple »).
    engine.RegisterInterfaceFunction("Exemple", [](Engine&) { Log::Info("bouton d'interface : fonction Exemple appelée"); });
}

FLUX_GAME_MODULE_EXPORT void FluxUnregisterGame(Flux::Engine& engine) {
    // Rien du jeu ne doit survivre au déchargement de la DLL : ni son code (Update), ni ses entités.
    // La scène et les composants enregistrés sont vidés ensuite par le moteur (GameModule::Unregister).
    engine.SetGameUpdate(nullptr);
    engine.UnregisterInterfaceFunction("Exemple");
}
