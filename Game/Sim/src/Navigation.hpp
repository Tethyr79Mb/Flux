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

    struct NavigationState
    {
        Flux::InputActionId autopilotAction = Flux::NoInputAction; // leurs bits dans PlayerCommand
        Flux::InputActionId warpAction = Flux::NoInputAction;
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

        void DeclareActions(NavigationState& state, Flux::CommandActions& actions);
        // Étape BeforePhysics, après le pilote et avant l'IA de combat : amarrage, warp, et leurs messages.
        void Travel(Flux::Simulation& simulation, SimState& state, double deltaSeconds);
        // Scène remplacée : destinations et objectif oubliés.
        void Reset(NavigationState& state);
        // Objectif proposé comme destination du warp (une mission), sous ce nom ; nullopt le retire.
        void SetWarpObjective(NavigationState& state, std::string name, std::optional<Flux::Math::WorldPosition> position);
        std::string GetDestinationName(const Flux::Scene& scene, const NavigationState& state, const Warp::Destination& destination);
    }
}
