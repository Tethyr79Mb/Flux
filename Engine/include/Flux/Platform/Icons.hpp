#pragma once

#include "Flux/Assets/Image.hpp"
#include "Flux/Export.hpp"

#include <optional>
#include <string_view>

// Icônes du système, pour les explorateurs de fichiers de l'éditeur.
namespace Flux::Platform {
    // Icône que l'explorateur de Windows montre pour ce type de fichier (elle dépend des logiciels installés),
    // ou pour un dossier si extension est vide. extension : point compris (".png"). 48 pixels de côté.
    // Le fichier n'est pas lu : seule l'extension compte. nullopt si Windows n'en donne pas.
    FLUX_API std::optional<ImageData> GetShellIcon(std::string_view extension);
}
