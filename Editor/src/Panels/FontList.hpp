#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    // Polices du projet : les .ttf et .otf rangées dans le dossier Polices/ des assets (sous-dossiers compris).
    // C'est la liste proposée pour un texte, dans l'inspecteur et dans la barre d'outils.
    inline constexpr std::string_view FontsFolder = "Polices";

    // Chemins relatifs au dossier des assets (« Polices/Orbitron.ttf »), triés ; vide sans projet. Le dossier est relu
    // au plus une fois par seconde : une police copiée dans l'explorateur apparaît sans redémarrer.
    const std::vector<std::string>& GetProjectFonts(const Flux::Engine& engine);
    // Nom affiché : le nom du fichier sans extension ni axes de variation (« Orbitron[wght] » donne « Orbitron ») ;
    // vide : « Arimo », la police de l'interface.
    std::string GetFontLabel(std::string_view font);
    // Liste déroulante de l'inspecteur : Arimo (par défaut), les polices du projet, puis « Ajouter une police... »
    // (choisie dans l'explorateur, copiée dans Polices/ si elle vient d'ailleurs). Retourne true quand font change.
    bool FontChoice(Flux::Engine& engine, std::string_view label, std::string& font);
}
