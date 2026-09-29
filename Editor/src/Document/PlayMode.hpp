#pragma once

#include <Flux/Graphics/Camera.hpp>

#include <string>

namespace Flux {
    class Engine;

    namespace UI {
        class Context;
    }
}

namespace FluxEditor {
    // Mode Jouer : le jeu tourne sur une copie de la scène éditée, restaurée telle quelle à l'arrêt.
    // Hors du jeu, le module de jeu n'est pas mis à jour : la scène ne bouge que sous les mains de l'utilisateur.
    class PlayMode {
        public:
        // Boutons Jouer / Pause / Arrêter, centrés dans la barre de menus.
        void DrawToolbar(Flux::UI::Context& ui, Flux::Engine& engine);
        // F5 : jouer ou arrêter ; F6 : pause.
        void HandleShortcuts(Flux::Engine& engine);

        void Start(Flux::Engine& engine);
        void Stop(Flux::Engine& engine);
        void TogglePause(Flux::Engine& engine);
        bool IsPlaying() const { return m_playing; }

        private:
        bool m_playing = false;
        std::string m_savedScene; // scène au lancement, au format du fichier (identifiants compris)
        Flux::Camera m_savedCamera;
    };
}
