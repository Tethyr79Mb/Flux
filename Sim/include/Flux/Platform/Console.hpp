#pragma once

#include "Flux/SimExport.hpp"

namespace Flux::Platform {
    // Les messages du moteur sont en UTF-8, mais la console Windows affiche par défaut dans la page de code OEM
    // (850 en français) : « scène » y devient « scÃ¨ne ». À appeler en tout début de main().
    FLUX_SIM_API void UseUtf8Console();

    // Fermeture de la console (sa croix, Alt+F4 quand elle a le focus, Ctrl+C) : sans rien faire, Windows tue le
    // processus aussitôt, sans fin de boucle ni enregistrement. Une fois CatchConsoleClose appelé, la fermeture est
    // notée (ConsoleCloseRequested, lu par la boucle du moteur ou du serveur) et Windows attend ConsoleCloseHandled,
    // 4 s au plus. Sous Linux, Ctrl+C et l'arrêt demandé par le système (SIGINT, SIGTERM, SIGHUP) sont notés de même.
    FLUX_SIM_API void CatchConsoleClose();
    FLUX_SIM_API bool ConsoleCloseRequested();
    FLUX_SIM_API void ConsoleCloseHandled();
}
