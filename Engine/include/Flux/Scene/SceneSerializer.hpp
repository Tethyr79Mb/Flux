#pragma once

#include "Flux/Export.hpp"

#include "Flux/Scene/Components.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace Flux {
    class ComponentRegistry;
    class Scene;

    // Scènes au format JSON (.scene.json), lisibles et modifiables à la main.
    // Les entités sont écrites parents d'abord, dans l'ordre de la hiérarchie ; leurs liens passent par leur Uuid.
    // Chaque composant enregistré dans components est un objet de ses champs, sous sa clé ("transform": {...}).
    // Un composant de type inconnu (module de jeu absent) est conservé tel quel et réécrit à la sauvegarde.
    namespace SceneSerializer {
        // À incrémenter à chaque changement du format ; Load refuse un fichier plus récent que le moteur.
        inline constexpr int FormatVersion = 1;

        FLUX_API std::string SaveToString(const Scene& scene, const ComponentRegistry& components);
        // Remplace tout le contenu de scene. En cas d'erreur, scene n'est pas modifiée.
        FLUX_API std::expected<void, std::string> LoadFromString(Scene& scene, const ComponentRegistry& components,
                                                                 std::string_view json);

        // Instantané d'une entité (annuler / refaire) : ses données au format du fichier, sa place parmi ses frères,
        // et celles de tous ses descendants si withDescendants.
        FLUX_API std::string SaveEntity(const Scene& scene, const ComponentRegistry& components, Entity entity,
                                        bool withDescendants);
        // Remet les entités d'un instantané dans leur état : recréées (même identifiant, même place) si elles
        // n'existent plus, sinon nom, parent, secteur et composants remplacés. Le reste de la scène n'est pas touché.
        // En cas d'erreur (parent disparu...), scene n'est pas modifiée.
        FLUX_API std::expected<void, std::string> RestoreEntity(Scene& scene, const ComponentRegistry& components,
                                                                std::string_view snapshot);

        // Écrit d'abord un fichier temporaire : une coupure pendant la sauvegarde n'abîme pas l'ancien fichier.
        FLUX_API std::expected<void, std::string> Save(const Scene& scene, const ComponentRegistry& components,
                                                       const std::filesystem::path& path);
        FLUX_API std::expected<void, std::string> Load(Scene& scene, const ComponentRegistry& components,
                                                       const std::filesystem::path& path);
    }
}
