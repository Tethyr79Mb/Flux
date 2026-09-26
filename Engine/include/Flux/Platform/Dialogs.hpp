#pragma once

#include "Flux/Export.hpp"

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
    FLUX_API void ShowError(const Window& owner, std::string_view title, std::string_view message);
}
