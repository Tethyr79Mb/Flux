#include "Flux/Platform/Filesystem.hpp"

#include <system_error>

namespace Flux::Platform {
    // Linux : le lien /proc/self/exe donne le chemin de l'exécutable (serveur sans rendu, CI).
    std::filesystem::path GetExecutableDirectory() {
        std::error_code error;
        const std::filesystem::path executable = std::filesystem::read_symlink("/proc/self/exe", error);
        if (error) {
            return std::filesystem::current_path();
        }
        return executable.parent_path();
    }
}
