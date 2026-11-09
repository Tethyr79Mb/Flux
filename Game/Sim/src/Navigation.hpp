#pragma once

#include "Warp.hpp"

#include <Flux/Math/WorldPosition.hpp>
#include <Flux/Scene/PlayerCommand.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Flux {
    class Scene;
    class Simulation;
}

// Navigation du vaisseau piloté : pilote automatique d'amarrage (L), warp vers la destination visée (J). Les briques
// sont dans Docking.hpp et Warp.hpp ; l'affichage des destinations est au client (NavigationHud.cpp).
namespace Game {
    struct SimState;
    struct PlayerStateComponent;

    struct NavigationActions
    {
        Flux::InputActionId autopilot = Flux::NoInputAction; // leurs bits dans PlayerCommand
        Flux::InputActionId warp = Flux::NoInputAction;
    };

    // Navigation d'un joueur (PlayerStateComponent).
    struct NavigationState
    {
        // Destinations du vaisseau piloté (recalculées à chaque pas, mémoire gardée), celle que vise son nez (-1 :
        // aucune), le nom de celle vers laquelle il file (vide hors warp), l'objectif de mission et la ligne
        // d'amarrage des infos de vol.
        std::vector<Warp::Destination> destinations;
        int aim = -1;
        std::string warpName;
        std::string objectiveName;
        std::optional<Flux::Math::WorldPosition> objective;
        std::string info;
    };

    namespace Navigation {
        inline constexpr std::string_view AutopilotAction = "Pilote automatique";
        inline constexpr std::string_view WarpAction = "Warp";

        void DeclareActions(NavigationActions& actions, Flux::CommandActions& commandActions);
        // Étape BeforePhysics, après le pilote et avant l'IA de combat : amarrage, warp, et leurs messages, pour
        // chaque joueur.
        void Travel(Flux::Simulation& simulation, const SimState& state, double deltaSeconds);
        // Objectif proposé comme destination du warp (une mission), sous ce nom ; nullopt le retire.
        void SetWarpObjective(NavigationState& state, std::string name, std::optional<Flux::Math::WorldPosition> position);
        std::string GetDestinationName(const Flux::Scene& scene, const NavigationState& state, const Warp::Destination& destination);
    }
}
