#pragma once

#include <filesystem>

namespace Flux {
    class Engine;

    namespace UI {
        class Context;
    }
}

namespace FluxEditor {
    // Disposition des panneaux de l'éditeur : ancrés autour de la vue 3D, retrouvés d'un lancement à l'autre.
    class EditorLayout {
        public:
        // Enregistre la disposition (à côté de FluxEditor.exe), si elle a été chargée.
        void Save(Flux::Engine& engine) const;

        // Menu « Fenêtre » de la barre de menus.
        void DrawWindowMenu(Flux::UI::Context& ui);
        // Pose l'ancrage sous la barre de menus et y cale la vue 3D ; avant les panneaux de l'éditeur.
        // La première fois, charge la disposition enregistrée, ou celle par défaut.
        void Update(Flux::Engine& engine);

        private:
        void Load(Flux::UI::Context& ui);
        static void BuildDefault(Flux::UI::Context& ui);
        static std::filesystem::path FilePath();

        bool m_loaded = false;
    };
}
