#pragma once

#include "Flux/Export.hpp"

#include <filesystem>

namespace Flux::Platform {
    FLUX_API std::filesystem::path GetExecutableDirectory();
}
