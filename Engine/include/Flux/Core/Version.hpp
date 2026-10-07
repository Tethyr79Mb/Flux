#pragma once

#include "Flux/Export.hpp"

#include <string_view>

namespace Flux {
    // Version de cette compilation (cmake/FluxVersion.cmake) : le numéro choisi dans CMakeLists.txt (project), le
    // commit Git (7 caractères, suivi de « + » si des fichiers avaient changé, « ? » sans Git), et les deux :
    // « 0.1.0 (9d84ca7) ».
    FLUX_API std::string_view GetVersion();
    FLUX_API std::string_view GetCommit();
    FLUX_API std::string_view GetFullVersion();
}
