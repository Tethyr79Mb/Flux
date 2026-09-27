#pragma once

#include <Flux/Core/Uuid.hpp>
#include <Flux/Math/Vector3.hpp>
#include <Flux/Math/WorldPosition.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/UI/Context.hpp>

#include <utility>
#include <vector>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    class SceneEditor;

    // Vue 3D de l'éditeur : clic gauche pour sélectionner un objet, contour de la sélection,
    // et gizmo de déplacement (une flèche par axe du monde, à glisser).
    class SceneViewport {
        public:
        // view : rectangle de la vue 3D (retourné par EditorLayout::Update).
        void Update(Flux::Engine& engine, SceneEditor& sceneEditor, const Flux::UI::Rect& view);
        // Vrai si la scène a été modifiée (déplacement au gizmo) depuis le dernier appel.
        bool TakeModified() { return std::exchange(m_modified, false); }
        // Une flèche du gizmo est tenue : le déplacement n'est pas fini.
        bool IsDragging() const { return m_drag.axis >= 0; }

        private:
        // Glissement d'une flèche du gizmo en cours.
        struct Drag
        {
            int axis = -1; // -1 : pas de glissement
            Flux::Uuid entity;
            Flux::Math::Vector3d startLocalPosition;  // à restaurer si Échap annule
            Flux::Math::Vector3d startPoint;          // origine du gizmo, relative à startCamera
            Flux::Math::WorldPosition startCamera;    // la caméra peut bouger pendant le glissement (clic droit)
            double startParameter = 0.0;              // point de l'axe saisi, en mètres depuis startPoint
        };

        Drag m_drag;
        int m_hoveredAxis = -1;
        bool m_modified = false;
        std::vector<Flux::RelativeTransform> m_transforms; // gardé d'une frame à l'autre : pas d'allocation
    };
}
