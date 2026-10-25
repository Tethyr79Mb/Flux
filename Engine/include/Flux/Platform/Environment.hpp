#pragma once

#include "Flux/Export.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace Flux::Platform {
    // Variable d'environnement (réglages de diagnostic : FLUX_MESURES...), en UTF-8 ; nullopt si elle n'existe pas.
    // Remplace std::getenv, que MSVC déconseille (C4996) et qui ne lit pas les accents.
    FLUX_API std::optional<std::string> ReadEnvironment(std::string_view name);
}
