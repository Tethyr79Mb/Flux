#pragma once

#include <Flux/Math/WorldPosition.hpp>

#include <optional>
#include <string>

namespace Flux {
    class Engine;
}

// Navigation du vaisseau piloté : pilote automatique d'amarrage (L), warp vers la destination visée (J), leurs
// affichages. Les briques sont dans Docking.hpp et Warp.hpp.
namespace Game {
    void RegisterNavigation(Flux::Engine& engine);
    void UnregisterNavigation(Flux::Engine& engine);

    // Objectif proposé comme destination du warp (une mission), sous ce nom ; nullopt le retire. Effacé avec la scène.
    void SetWarpObjective(std::string name, std::optional<Flux::Math::WorldPosition> position);
}
