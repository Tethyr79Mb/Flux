#pragma once

#include "Flux/Export.hpp"

#include <cstdint>

namespace Flux::Platform {
    // Mémoire du processus, en octets. privateBytes : ce que le processus a réservé pour lui seul (la colonne
    // « Mémoire » du Gestionnaire des tâches en est proche) ; workingSet : ce qui est en RAM en ce moment, partagé compris.
    struct ProcessMemory
    {
        std::uint64_t privateBytes = 0;
        std::uint64_t workingSet = 0;
    };

    FLUX_API ProcessMemory GetProcessMemory();
}
