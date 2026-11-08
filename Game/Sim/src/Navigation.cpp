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

        void ToggleAutopilot(Scene& scene, SimState& state, Entity ship) {
            Warp::Stop(scene, ship); // L en warp : il en sort et cherche un quai
            state.navigation.warpName.clear();
            switch (Docking::Toggle(scene, ship)) {
            case Docking::ToggleResult::Docking:
            case Docking::ToggleResult::Leaving: break; // l'étape s'affiche dans les infos de vol
            case Docking::ToggleResult::Cancelled: state.notices.emplace_back("Pilote automatique coupé"); break;
            case Docking::ToggleResult::NoDock:
                state.notices.push_back(std::format("Aucun quai libre à moins de {:.0f} km", Docking::Range / 1000.0));
                break;
            }
        }

        // Destinations à jour, visée, et J : partir vers la destination visée, ou couper le warp.
        void UpdateWarp(Scene& scene, SimState& state, Entity ship, bool toggle) {
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
                state.notices.push_back("Sortie du warp : " + navigation.warpName);
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
                state.notices.emplace_back("Warp coupé");
                return;
            }
            if (navigation.aim < 0) {
                state.notices.emplace_back("Warp : tournez le nez vers une destination");
                return;
            }
            const Warp::Destination& destination = navigation.destinations[static_cast<std::size_t>(navigation.aim)];
            switch (Warp::Engage(scene, ship, destination)) {
            case Warp::EngageResult::Started: navigation.warpName = GetDestinationName(scene, navigation, destination); break;
            case Warp::EngageResult::Unavailable:
                state.notices.emplace_back("Warp impossible ici (posé, amarré, près d'une planète, sans moteur de warp)");
                break;
            case Warp::EngageResult::TooClose: state.notices.emplace_back("Trop près pour le warp : mode voyage (Tab)"); break;
            }
        }
    }

    void DeclareActions(NavigationState& state, CommandActions& actions) {
        state.autopilotAction = actions.Declare(AutopilotAction);
        state.warpAction = actions.Declare(WarpAction);
    }

    void Travel(Simulation& simulation, SimState& state, double deltaSeconds) {
        Scene& scene = simulation.GetScene();
        const Entity player = simulation.GetPlayer();
        const Entity ship = player != NullEntity ? Ships::GetPilotedShip(scene, player) : NullEntity;
        const PlayerCommand& command = simulation.GetPlayerCommand(player);
        if (ship != NullEntity && command.WasPressed(state.navigation.autopilotAction)) {
            ToggleAutopilot(scene, state, ship);
        }
        Docking::Update(scene, deltaSeconds);
        UpdateWarp(scene, state, ship, ship != NullEntity && command.WasPressed(state.navigation.warpAction));
        Warp::Update(scene, deltaSeconds);
        state.navigation.info = ship != NullEntity ? Docking::Describe(scene, ship) : std::string{};
    }

    void Reset(NavigationState& state) {
        state.destinations.clear();
        state.aim = -1;
        state.warpName.clear();
        state.objective.reset();
        state.objectiveName.clear();
        state.info.clear();
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
