#pragma once

#include <Flux/Core/Uuid.hpp>
#include <Flux/Scene/Components.hpp>

#include <string>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    class SceneEditor;
    class UndoHistory;

    // Fenêtre « Monde » (bouton Monde de la barre de menus) : gravité, air et chute max de la scène ouverte. Ils sont
    // rangés dans l'objet Monde de la scène (WorldComponent, un seul compte) ; sans lui, la fenêtre montre les
    // valeurs par défaut et le crée à la première modification. Chaque geste est un pas d'annulation.
    class WorldWindow {
        public:
        void Toggle() { m_open = !m_open; }
        bool IsOpen() const { return m_open; }
        // readOnly : mode Jouer, on regarde sans modifier.
        void Draw(Flux::Engine& engine, SceneEditor& sceneEditor, UndoHistory& history, bool readOnly);

        private:
        bool m_open = false;
        Flux::WorldComponent m_defaults; // montré quand la scène n'a pas d'objet Monde
        // Geste en cours (champ tenu) : instantané de l'objet avant lui (vide : il n'existait pas), enregistré au lâcher.
        Flux::Uuid m_gestureEntity;
        std::string m_before;
        bool m_gesture = false;
    };
}
