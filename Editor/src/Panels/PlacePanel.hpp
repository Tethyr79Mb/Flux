#pragma once

#include <Flux/Math/Vector2.hpp>
#include <Flux/UI/Context.hpp>

#include <optional>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    class SceneEditor;
    class UndoHistory;

    // Panneau « Placer » (le « Place Actors » d'Unreal) : les objets qu'on peut ajouter à la scène, par catégorie.
    // Un clic crée l'objet (devant la caméra, ou au centre de l'écran pour un texte ou une image) ; le glisser dans la vue
    // le pose sous la souris. Un modèle 3D ou un personnage demande d'abord son fichier. L'objet créé est sélectionné, et Ctrl+Z le retire.
    class PlacePanel {
        public:
        // view : rectangle de la vue 3D (retourné par EditorLayout::Update).
        void Update(Flux::Engine& engine, SceneEditor& sceneEditor, UndoHistory& history, const Flux::UI::Rect& view);

        private:
        enum class Item { Empty, Cube, Model, ScreenText, ScreenImage, SceneChange, Character, Ship, Fighter, Light, Spot, Sky, Interior, Interface, Sound };

        // drop : point de la vue où l'objet a été lâché ; sans lui, la place par défaut.
        static void Place(Flux::Engine& engine, SceneEditor& sceneEditor, UndoHistory& history, Item item,
                          std::optional<Flux::Math::Vector2f> drop, const Flux::UI::Rect& view);

        std::optional<Item> m_held; // bouton tenu
        Flux::Math::Vector2f m_pressMouse;
        bool m_dragging = false;    // la souris s'est éloignée du bouton tenu : glisser-déposer en cours
    };
}
