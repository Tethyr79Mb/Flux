#pragma once

#include <Flux/Scene/Components.hpp>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    class SceneEditor;
    class UndoHistory;

    // Groupe « Texte » de la barre d'outils : poser un texte à l'écran, puis sa police (celles du dossier Polices/
    // du projet), sa taille et sa couleur. Avec un texte sélectionné, les choix s'appliquent à lui, un pas
    // d'annulation chacun ; sinon, ils servent au prochain texte posé. Les mêmes réglages restent dans l'inspecteur.
    class TextTools {
        public:
        // readOnly : mode Jouer, rien à poser ni à modifier.
        void DrawToolBarItems(Flux::Engine& engine, SceneEditor& sceneEditor, UndoHistory& history, bool readOnly);

        private:
        void Place(Flux::Engine& engine, SceneEditor& sceneEditor, UndoHistory& history);

        Flux::ScreenTextComponent m_next; // réglages du prochain texte posé
    };
}
