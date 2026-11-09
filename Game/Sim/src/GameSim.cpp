#include "GameSim.hpp"

#include "Combat.hpp"
#include "CombatAI.hpp"
#include "Docking.hpp"
#include "MissionGiver.hpp"
#include "Warp.hpp"
#include "Weapons.hpp"

#include <Flux/Core/Log.hpp>
#include <Flux/Core/Simulation.hpp>
#include <Flux/Scene/ComponentRegistry.hpp>
#include <Flux/Scene/Scene.hpp>

#include <array>
#include <string>
#include <utility>
#include <string_view>

namespace Game {
    namespace {
        using namespace Flux;

        // Premier composant du jeu : l'entité tourne sur elle-même (et emporte ses enfants). Enregistré ici :
        // sauvegardé, chargé et affiché dans l'inspecteur sans autre code.
        struct SpinComponent
        {
            Math::Vector3f axis{0.0f, 1.0f, 0.0f};
            float speed = 1.0f; // rad/s
        };

        // Toutes les entités qui ont SpinComponent, quel que soit leur nom : le système solaire de la démo, mais
        // aussi tout cube auquel on ajoute « Rotation continue » dans l'inspecteur.
        void Spin(Simulation& simulation, float speed, double deltaSeconds) {
            const float time = speed * static_cast<float>(deltaSeconds);
            simulation.GetScene().GetRegistry().view<const SpinComponent, TransformComponent>().each(
                [&](const SpinComponent& spin, TransformComponent& transform) {
                    const Math::Quaternionf step = Math::Quaternionf::FromAxisAngle(spin.axis, spin.speed * time);
                    transform.rotation = Math::Normalize(step * transform.rotation);
                });
        }

        void Damage(Simulation& simulation, SimState& state) {
            state.destroyed.clear();
            Weapons::ApplyHits(simulation.GetScene(), simulation.GetProjectileHits(), simulation.GetPlayers(), &state.destroyed);
            for (const std::string& name : state.destroyed) {
                Log::Info("« {} » détruit", name);
            }
        }

        // Dans l'ordre d'appel à chaque étape, le même qu'au temps du moteur.
        constexpr std::string_view PilotSystem = "Pilotage : commandes";
        constexpr std::string_view MissionSystem = "Missions : parler";
        constexpr std::string_view TravelSystem = "Navigation : amarrage et warp";
        constexpr std::string_view SteerSystem = "Combat : pilotage des ennemis";
        constexpr std::string_view DamageSystem = "Armes : dégâts";
        constexpr std::string_view FireSystem = "Armes du joueur : tir";
        constexpr std::string_view EnemyFireSystem = "Combat : tirs des ennemis";
        constexpr std::string_view SpinSystem = "Démo : rotations";
        constexpr std::string_view ResetSystem = "Jeu : oubli";
        constexpr std::array Systems{PilotSystem, MissionSystem, TravelSystem,    SteerSystem, DamageSystem,
                                     FireSystem,  EnemyFireSystem, SpinSystem, ResetSystem};
    }

    PlayerStateComponent& GetPlayerState(Scene& scene, Entity player) {
        return scene.GetRegistry().get_or_emplace<PlayerStateComponent>(player);
    }

    PlayerStateComponent* FindPlayerState(Scene& scene, Entity player) {
        return player != NullEntity && scene.IsValid(player) ? scene.GetRegistry().try_get<PlayerStateComponent>(player) : nullptr;
    }

    void AddNotice(PlayerStateComponent& state, std::string notice) {
        if (state.notices.size() >= PlayerStateComponent::MaxNotices) {
            state.notices.erase(state.notices.begin());
        }
        state.notices.push_back(std::move(notice));
    }

    std::shared_ptr<SimState> RegisterSimulation(Simulation& simulation) {
        ComponentRegistry& components = simulation.GetComponents();
        components.Register<SpinComponent>("spin", "Rotation continue")
            .Field("axis", "Axe", &SpinComponent::axis, {.speed = 0.01})
            .Field("speed", "Vitesse (rad/s)", &SpinComponent::speed, {.speed = 0.01, .min = -100.0, .max = 100.0, .decimals = 2});
        RegisterWeaponComponents(components);
        RegisterDock(components);
        RegisterWarpDrive(components);
        RegisterCombatAI(components);
        RegisterMissionGiver(components);

        auto state = std::make_shared<SimState>();
        CommandActions& actions = simulation.GetActions();
        state->pilot = Piloting::DeclareActions(actions);
        Navigation::DeclareActions(state->navigation, actions);
        PlayerWeapons::DeclareActions(state->weapons, actions);

        using Stage = Simulation::Stage;
        simulation.AddSystem(Stage::Interaction, std::string(PilotSystem),
                             [state](Simulation& s, double) { Piloting::Pilot(s, state->pilot); });
        simulation.AddSystem(Stage::Interaction, std::string(MissionSystem),
                             [](Simulation& s, double) { Missions::Interact(s); });
        simulation.AddSystem(Stage::BeforePhysics, std::string(TravelSystem),
                             [state](Simulation& s, double dt) { Navigation::Travel(s, *state, dt); });
        simulation.AddSystem(Stage::BeforePhysics, std::string(SteerSystem),
                             [state](Simulation& s, double dt) { Combat::Steer(s, state->combatTargets, dt); });
        // Les dégâts du pas avant les nouveaux tirs ; le joueur tire avant les ennemis.
        simulation.AddSystem(Stage::AfterPhysics, std::string(DamageSystem), [state](Simulation& s, double) { Damage(s, *state); });
        simulation.AddSystem(Stage::AfterPhysics, std::string(FireSystem),
                             [state](Simulation& s, double dt) { PlayerWeapons::Fire(s, *state, dt); });
        simulation.AddSystem(Stage::AfterPhysics, std::string(EnemyFireSystem), Combat::Fire);
        simulation.AddSystem(Stage::AfterPhysics, std::string(SpinSystem),
                             [state](Simulation& s, double dt) { Spin(s, state->spinSpeed, dt); });
        simulation.AddSystem(Stage::Reset, std::string(ResetSystem), [state](Simulation& s, double) {
            s.GetScene().GetRegistry().clear<PlayerStateComponent>(); // missions, navigation et messages des joueurs
            PlayerWeapons::Reset(s.GetScene(), state->weapons);
        });
        return state;
    }

    void UnregisterSimulation(Simulation& simulation) {
        for (const std::string_view system : Systems) {
            simulation.RemoveSystem(system);
        }
        // Leurs types sont dans le module déchargé.
        simulation.GetScene().GetRegistry().clear<PilotWeaponsComponent>();
        simulation.GetScene().GetRegistry().clear<PlayerStateComponent>();
    }
}
