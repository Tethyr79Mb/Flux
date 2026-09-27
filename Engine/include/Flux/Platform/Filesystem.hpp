#pragma once

#include "Flux/Export.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace Flux::Platform {
    FLUX_API std::filesystem::path GetExecutableDirectory();

    // path::string() et le constructeur depuis std::string passent par la page de code Windows (les accents
    // seraient perdus) : conversions UTF-8 explicites, pour les messages, les fichiers JSON et l'interface.
    inline std::string ToUtf8(const std::filesystem::path& path) {
        const std::u8string text = path.u8string();
        return {text.begin(), text.end()};
    }

    // Avec des « / » : le même texte sous Windows et ailleurs (chemins enregistrés dans les fichiers).
    inline std::string ToGenericUtf8(const std::filesystem::path& path) {
        const std::u8string text = path.generic_u8string();
        return {text.begin(), text.end()};
    }

    inline std::filesystem::path PathFromUtf8(std::string_view text) {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }
}
