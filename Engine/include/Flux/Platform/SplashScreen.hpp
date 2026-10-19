#pragma once

#include "Flux/Export.hpp"

#include <filesystem>
#include <memory>

namespace Flux::Platform {
    // Écran d'accueil : une image seule au milieu de l'écran (sans cadre, au-dessus des autres fenêtres, transparente
    // là où l'image l'est), le temps que le programme démarre. Fermé par Close ou à sa destruction.
    class FLUX_API SplashScreen {
        public:
        SplashScreen();
        ~SplashScreen();

        SplashScreen(const SplashScreen&) = delete;
        SplashScreen& operator=(const SplashScreen&) = delete;

        // false si l'image ne se lit pas (rien n'est affiché) : le programme démarre quand même.
        bool Show(const std::filesystem::path& image);
        void Close();

        private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
