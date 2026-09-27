#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Color.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Flux {
    class Window;
}

// Boîtes de dialogue natives du système (fenêtres modales : la boucle du moteur attend la réponse).
namespace Flux::Platform {
    // Type de fichier proposé : description affichée et extension, point compris (".scene.json").
    // Ouverture seulement : plusieurs extensions séparées par « ; » (".png;.jpg").
    struct FileType
    {
        std::string description;
        std::string extension;
    };

    // Chemin choisi, ou nullopt si l'utilisateur annule.
    FLUX_API std::optional<std::filesystem::path> ShowOpenFileDialog(const Window& owner, std::string_view title,
                                                                    const FileType& type,
                                                                    const std::filesystem::path& initialDirectory);
    // L'extension est ajoutée si l'utilisateur ne l'a pas tapée ; remplacer un fichier existant demande confirmation.
    FLUX_API std::optional<std::filesystem::path> ShowSaveFileDialog(const Window& owner, std::string_view title,
                                                                    const FileType& type,
                                                                    const std::filesystem::path& initialDirectory,
                                                                    std::string_view initialName);

    enum class Answer { Yes, No, Cancel };
    // Question à trois réponses (Oui, Non, Annuler), par exemple avant de perdre des modifications.
    FLUX_API Answer AskYesNoCancel(const Window& owner, std::string_view title, std::string_view question);
    FLUX_API bool AskYesNo(const Window& owner, std::string_view title, std::string_view question);
    FLUX_API void ShowError(const Window& owner, std::string_view title, std::string_view message);
    // Sélecteur de couleur de Windows (nuancier complet, couleurs personnalisées gardées jusqu'à la fermeture).
    // Couleur choisie (alpha inchangé), ou nullopt si l'utilisateur annule.
    FLUX_API std::optional<Math::LinearColor> ShowColorDialog(const Window& owner, const Math::LinearColor& initial);

    // Fichier ou dossier envoyé à la corbeille (récupérable), sans autre confirmation que celle de l'appelant.
    FLUX_API std::expected<void, std::string> MoveToRecycleBin(const std::filesystem::path& path);
    // Ouvre l'explorateur sur le dossier du fichier, fichier sélectionné.
    FLUX_API void ShowInExplorer(const std::filesystem::path& path);
}
