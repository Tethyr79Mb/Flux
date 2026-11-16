#pragma once

#include <Flux/UI/Context.hpp>

#include <filesystem>

namespace Flux {
    class Engine;

}

namespace FluxEditor {
    // Disposition des panneaux de l'éditeur : ancrés autour de la vue 3D, retrouvés d'un lancement à l'autre.
    class EditorLayout {
        public:
        // Enregistre la disposition (à côté de FluxEditor.exe), si elle a été chargée.
        void Save(Flux::Engine& engine) const;

        // Menu « Fenêtre » de la barre de menus.
        void DrawWindowMenu(Flux::Engine& engine);
        // Vue seule : la vue 3D occupe tout l'écran, sans panneaux, barres ni aides de l'éditeur (essais, captures).
        // F11 l'ouvre avec le plein écran de la fenêtre et la ferme ; Échap la ferme hors du mode Jouer (en jeu, Échap
        // appartient au jeu). Alt+Entrée ne bascule que le plein écran, panneaux gardés. Au début de la frame.
        void HandleShortcuts(Flux::Engine& engine, bool playing);
        bool IsViewOnly() const { return m_viewOnly; }
        // Pose l'ancrage entre la barre de menus et la barre d'état et y cale la vue 3D ; avant les panneaux de
        // l'éditeur. La première fois, charge la disposition enregistrée, ou celle par défaut. Retourne le rectangle de
        // la vue 3D : toute la fenêtre en vue seule.
        Flux::UI::Rect Update(Flux::Engine& engine);

        private:
        void Load(Flux::UI::Context& ui);
        static void BuildDefault(Flux::UI::Context& ui);
        static std::filesystem::path FilePath();

        void SetViewOnly(Flux::Engine& engine, bool viewOnly);

        bool m_loaded = false;
        bool m_viewOnly = false;
    };
}
