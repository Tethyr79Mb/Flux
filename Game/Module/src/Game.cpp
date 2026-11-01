#include "Game.hpp"

#include "Combat.hpp"
#include "CombatAI.hpp"
#include "MissionGiver.hpp"
#include "Missions.hpp"
#include "PlaceItems.hpp"
#include "PlayerWeapons.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Input/InputActions.hpp>
#include <Flux/Scene/ComponentRegistry.hpp>
#include <Flux/Scene/Scene.hpp>

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

    // Multiplie la vitesse de toutes les rotations (liée à « Vitesse de la démo » pour les interfaces).
    float g_timeScale = 1.0f;

    void JumpSector(Engine& engine) {
        // Même point de vue, un autre secteur.
        Camera& camera = engine.GetCamera();
        camera.position.sector.z = camera.position.sector.z == 0 ? FarSectorZ : 0;
    }

    void Update(Engine& engine, double deltaSeconds) {
        // Toutes les entités qui ont SpinComponent, quel que soit leur nom : le système solaire de la démo,
        // mais aussi tout cube auquel on ajoute « Rotation continue » dans l'inspecteur.
        const float time = g_timeScale * static_cast<float>(deltaSeconds);
        engine.GetScene().GetRegistry().view<const SpinComponent, TransformComponent>().each(
            [&](const SpinComponent& spin, TransformComponent& transform) {
                const Math::Quaternionf step = Math::Quaternionf::FromAxisAngle(spin.axis, spin.speed * time);
                transform.rotation = Math::Normalize(step * transform.rotation);
            });

        // Action d'entrée du jeu (touche T par défaut, réassignable) ; muette pendant la saisie dans un champ.
        if (engine.WasActionPressed("Saut d'une année-lumière")) {
            JumpSector(engine);
        }
    }
}

namespace Game {
    using namespace Flux;

    void Register(Engine& engine) {
        engine.GetComponents()
            .Register<SpinComponent>("spin", "Rotation continue")
            .Field("axis", "Axe", &SpinComponent::axis, {.speed = 0.01})
            .Field("speed", "Vitesse (rad/s)", &SpinComponent::speed, {.speed = 0.01, .min = -100.0, .max = 100.0, .decimals = 2});
        RegisterCombatAI(engine.GetComponents());
        RegisterMissionGiver(engine.GetComponents());
        engine.SetGameUpdate(Update);
        RegisterMissions(engine);
        RegisterPlayerWeapons(engine); // avant les ennemis : même ordre de tir qu'avant
        RegisterCombat(engine);
        RegisterPlaceItems(engine.GetEditorExtensions()); // retirés par le moteur au déchargement
        engine.GetInputActions().Add("Saut d'une année-lumière", "Démo", Key::T);
        // Exemple d'action « Fonction C++ » d'un bouton d'interface (cible : « Exemple »).
        engine.RegisterInterfaceFunction("Exemple", [](Engine&) { Log::Info("bouton d'interface : fonction Exemple appelée"); });
        // Exemple de liaison : un curseur lié à « Vitesse de la démo » la change, un texte ou une barre l'affiche.
        engine.BindInterfaceValue(
            "Vitesse de la démo", [] { return static_cast<double>(g_timeScale); },
            [](double value) { g_timeScale = static_cast<float>(value); });
    }

    void Unregister(Engine& engine) {
        engine.SetGameUpdate(nullptr);
        UnregisterMissions(engine);
        UnregisterCombat(engine);
        UnregisterPlayerWeapons(engine);
        engine.UnregisterInterfaceFunction("Exemple");
        engine.UnbindInterfaceValue("Vitesse de la démo");
        engine.GetInputActions().Remove("Saut d'une année-lumière");
    }
}
