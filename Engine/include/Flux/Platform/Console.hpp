#pragma once

#include "Flux/Export.hpp"

namespace Flux::Platform {
    // Les messages du moteur sont en UTF-8, mais la console Windows affiche par défaut dans la page de code OEM
    // (850 en français) : « scène » y devient « scÃ¨ne ». À appeler en tout début de main().
    FLUX_API void UseUtf8Console();
}
