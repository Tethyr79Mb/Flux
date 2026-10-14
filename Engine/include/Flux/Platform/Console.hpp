#pragma once

#include "Flux/Export.hpp"

namespace Flux::Platform {
    // Les messages du moteur sont en UTF-8, mais la console Windows affiche par défaut dans la page de code OEM
    // (850 en français) : « scène » y devient « scÃ¨ne ». À appeler en tout début de main().
    FLUX_API void UseUtf8Console();

    // Fermeture de la console (sa croix, Alt+F4 quand elle a le focus, Ctrl+C) : sans rien faire, Windows tue le
    // processus aussitôt, sans fin de boucle ni enregistrement. Une fois CatchConsoleClose appelé, la fermeture est
    // notée (ConsoleCloseRequested, lu par la boucle du moteur) et Windows attend ConsoleCloseHandled, 4 s au plus.
    FLUX_API void CatchConsoleClose();
    FLUX_API bool ConsoleCloseRequested();
    FLUX_API void ConsoleCloseHandled();
}
