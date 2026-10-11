#pragma once

#include "Flux/Export.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

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

    // Tout le fichier d'un bloc dans `bytes` ; false s'il ne s'ouvre pas ou ne se lit pas. Plutôt qu'octet par octet
    // (istreambuf_iterator) : quelques Mo prennent alors des secondes en Debug.
    inline bool ReadFileBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            return false;
        }
        const std::streamoff length = file.tellg();
        if (length < 0) {
            return false;
        }
        bytes.resize(static_cast<std::size_t>(length));
        file.seekg(0, std::ios::beg);
        return static_cast<bool>(file.read(reinterpret_cast<char*>(bytes.data()), length));
    }
}
