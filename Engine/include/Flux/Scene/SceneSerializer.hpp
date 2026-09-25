#pragma once

#include "Flux/Export.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace Flux {
    class Scene;

    // Scènes au format JSON (.scene.json), lisibles et modifiables à la main.
    // Les entités sont écrites parents d'abord, dans l'ordre de la hiérarchie ; leurs liens passent par leur Uuid.
    namespace SceneSerializer {
        // À incrémenter à chaque changement du format ; Load refuse un fichier plus récent que le moteur.
        inline constexpr int FormatVersion = 1;

        FLUX_API std::string SaveToString(const Scene& scene);
        // Remplace tout le contenu de scene. En cas d'erreur, scene n'est pas modifiée.
        FLUX_API std::expected<void, std::string> LoadFromString(Scene& scene, std::string_view json);

        // Écrit d'abord un fichier temporaire : une coupure pendant la sauvegarde n'abîme pas l'ancien fichier.
        FLUX_API std::expected<void, std::string> Save(const Scene& scene, const std::filesystem::path& path);
        FLUX_API std::expected<void, std::string> Load(Scene& scene, const std::filesystem::path& path);
    }
}
