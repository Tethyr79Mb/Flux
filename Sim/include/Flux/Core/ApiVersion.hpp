#pragma once

#include <cstdint>

namespace Flux {
    // À incrémenter à chaque changement incompatible du contrat entre moteur et jeu. À part de GameModule.hpp : la
    // simulation (FluxSim, sans fenêtre ni rendu) l'écrit dans les scènes sans connaître le chargement des modules.
    inline constexpr std::uint32_t GameModuleApiVersion = 85;
}
