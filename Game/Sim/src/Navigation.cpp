#include "Navigation.hpp"

#include "Docking.hpp"
#include "GameSim.hpp"

#include <Flux/Core/Simulation.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/Ships.hpp>

#include <format>

namespace Game::Navigation {
    namespace {
        using namespace Flux;

        void ToggleAutopilot(Scene& scene, PlayerStateComponent& state, Entity ship) {
            Warp::Stop(scene, ship); // L en warp : il en sort et cherche un quai
            state.navigation.warpName.clear();
            switch (Docking::Toggle(scene, ship)) {
            case Docking::ToggleResult::Docking:
            case Docking::ToggleResult::Leaving: break; // l'étape s'affiche dans les infos de vol
            case Docking::ToggleResult::Cancelled: AddNotice(state, "Pilote automatique coupé"); break;
            case Docking::ToggleResult::NoDock:
                AddNotice(state, std::format("Aucun quai libre à moins de {:.0f} km", Docking::Range / 1000.0));
                break;
            }
        }

        // Destinations à jour, visée, et J : partir vers la destination visée, ou couper le warp.
        void UpdateWarp(Scene& scene, PlayerStateComponent& state, Entity ship, bool toggle) {
            NavigationState& navigation = state.navigation;
            navigation.aim = -1;
            if (ship == NullEntity) {
                navigation.destinations.clear();
                return;
            }
            Warp::FindDestinations(scene, ship, navigation.destinations);
            if (navigation.objective) {
                navigation.destinations.push_back({NullEntity, *navigation.objective, Warp::StationExit});
            }
            const auto* drive = scene.TryGet<WarpDriveComponent>(ship);
            const bool warping = drive && drive->stage != WarpStage::Off;
            // Arrivé, ou arrêté par une planète sur le chemin.
            if (!warping && !navigation.warpName.empty()) {
                AddNotice(state, "Sortie du warp : " + navigation.warpName);
                navigation.warpName.clear();
            }
            if (!warping) {
                navigation.aim = Warp::Aim(scene, ship, navigation.destinations);
            }
            if (!toggle) {
                return;
            }
            if (warping) {
                Warp::Stop(scene, ship);
                navigation.warpName.clear();
                AddNotice(state, "Warp coupé");
                return;
            }
            if (navigation.aim < 0) {
                AddNotice(state, "Warp : tournez le nez vers une destination");
                return;
            }
            const Warp::Destination& destination = navigation.destinations[static_cast<std::size_t>(navigation.aim)];
            switch (Warp::Engage(scene, ship, destination)) {
            case Warp::EngageResult::Started: navigation.warpName = GetDestinationName(scene, navigation, destination); break;
            case Warp::EngageResult::Unavailable:
                AddNotice(state, "Warp impossible ici (posé, amarré, près d'une planète, sans moteur de warp)");
                break;
            case Warp::EngageResult::TooClose: AddNotice(state, "Trop près pour le warp : mode voyage (Tab)"); break;
            }
        }
    }

    void DeclareActions(NavigationActions& actions, CommandActions& commandActions) {
        actions.autopilot = commandActions.Declare(AutopilotAction);
        actions.warp = commandActions.Declare(WarpAction);
    }

    void Travel(Simulation& simulation, const SimState& state, double deltaSeconds) {
        Scene& scene = simulation.GetScene();
        // Dans cet ordre pour tous les joueurs : pilote automatique, amarrage, warp.
        for (const Entity player : simulation.GetPlayers()) {
            const Entity ship = Ships::GetPilotedShip(scene, player);
            if (ship != NullEntity && simulation.GetPlayerCommand(player).WasPressed(state.navigation.autopilot)) {
                ToggleAutopilot(scene, GetPlayerState(scene, player), ship);
            }
        }
        Docking::Update(scene, deltaSeconds);
        for (const Entity player : simulation.GetPlayers()) {
            const Entity ship = Ships::GetPilotedShip(scene, player);
            const bool toggle = ship != NullEntity && simulation.GetPlayerCommand(player).WasPressed(state.navigation.warp);
            UpdateWarp(scene, GetPlayerState(scene, player), ship, toggle);
        }
        Warp::Update(scene, deltaSeconds);
        for (const Entity player : simulation.GetPlayers()) {
            const Entity ship = Ships::GetPilotedShip(scene, player);
            GetPlayerState(scene, player).navigation.info = ship != NullEntity ? Docking::Describe(scene, ship) : std::string{};
        }
    }

    void SetWarpObjective(NavigationState& state, std::string name, std::optional<Math::WorldPosition> position) {
        state.objectiveName = position ? std::move(name) : std::string{};
        state.objective = position;
    }

    std::string GetDestinationName(const Scene& scene, const NavigationState& state, const Warp::Destination& destination) {
        if (destination.entity == NullEntity) {
            return state.objectiveName.empty() ? std::string("Objectif") : state.objectiveName;
        }
        const auto* name = scene.IsValid(destination.entity) ? scene.TryGet<NameComponent>(destination.entity) : nullptr;
        return name ? name->name : std::string("Destination");
    }
}
