#pragma once

#include <Flux/Core/Uuid.hpp>
#include <Flux/Math/Quaternion.hpp>
#include <Flux/Math/Vector3.hpp>
#include <Flux/Math/WorldPosition.hpp>
#include <Flux/Scene/Picking.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/ScreenUi.hpp>
#include <Flux/UI/Context.hpp>

#include <utility>
#include <vector>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    class SceneEditor;

    // Vue 3D de l'éditeur : clic gauche pour sélectionner un objet (une pièce fixe d'un vaisseau sélectionne le
    // vaisseau), contour de la sélection, et gizmo à glisser :
    // déplacement (une flèche par axe du monde), rotation (un anneau par axe du monde) ou échelle (un axe par axe de
    // l'objet, et le carré du centre pour les trois à la fois). Mode choisi par les boutons en haut à gauche de la
    // vue ou les touches W, E, R. Les textes et images à l'écran de la scène y sont dessinés comme dans le jeu, et se
    // déplacent en les glissant. Les lumières y sont montrées par une ampoule (sélection au clic) et, sélectionnées,
    // par leur portée.
    class SceneViewport {
        public:
        enum class GizmoMode { Move, Rotate, Scale };

        // view : rectangle de la vue 3D (retourné par EditorLayout::Update).
        void Update(Flux::Engine& engine, SceneEditor& sceneEditor, const Flux::UI::Rect& view);
        // Vrai si la scène a été modifiée (déplacement au gizmo, d'un texte ou d'une image) depuis le dernier appel.
        bool TakeModified() { return std::exchange(m_modified, false); }
        // Une flèche du gizmo, un texte ou une image est tenu : le déplacement n'est pas fini.
        bool IsDragging() const { return m_drag.axis >= 0 || m_screenDrag.active; }

        private:
        // Glissement d'une poignée du gizmo en cours.
        struct Drag
        {
            int axis = -1; // -1 : pas de glissement ; 0 à 2 : x, y, z ; 3 : échelle des trois axes (carré du centre)
            GizmoMode mode = GizmoMode::Move;
            Flux::Uuid entity;
            // État local de départ, à restaurer si Échap annule.
            Flux::Math::Vector3d startLocalPosition;
            Flux::Math::Quaternionf startLocalRotation;
            Flux::Math::Vector3f startLocalScale{1.0f, 1.0f, 1.0f};
            Flux::Math::Quaternionf startRotation;    // rotation dans le monde au départ (axes de l'échelle)
            Flux::Math::Quaternionf parentRotation;   // rotation du parent dans le monde (identité à la racine)
            Flux::Math::Vector3d startPoint;          // origine du gizmo, relative à startCamera
            Flux::Math::WorldPosition startCamera;    // la caméra peut bouger pendant le glissement (clic droit)
            double startParameter = 0.0;              // point de l'axe saisi, en mètres depuis startPoint
            Flux::Math::Vector2f startMouse;
            Flux::Math::Vector2f tangent;             // rotation : sens du glissement qui fait tourner, à l'écran
            float applied = 0.0f;                     // rotation : angle appliqué (par pas de 15° avec Ctrl)
        };

        void HandleShortcuts(Flux::Engine& engine);
        bool WantsFocus(Flux::Engine& engine) const;
        static void FocusOn(Flux::Engine& engine, const Flux::Scene& scene, Flux::Entity entity);
        // Boutons de mode en haut à gauche de la vue ; retourne true si la souris en a cliqué un.
        bool UpdateToolbar(Flux::Engine& engine, const Flux::UI::Rect& view, bool mouseInView);
        void DrawToolbar(Flux::Engine& engine, const Flux::UI::Rect& view);
        // Applique le glissement en cours à la transformation locale de l'entité.
        void UpdateDrag(Flux::Engine& engine, Flux::Entity selected, Flux::RelativeTransform& selectedTransform,
                        const Flux::RelativeTransform* parentTransform, const Flux::Ray& ray);

        // Glissement d'un élément à l'écran (texte, image) : son décalage suit la souris.
        struct ScreenDrag
        {
            bool active = false;
            Flux::Uuid entity;
            float startOffsetX = 0.0f; // à restaurer si Échap annule
            float startOffsetY = 0.0f;
            Flux::Math::Vector2f startMouse;
        };

        // Déplace l'élément tenu ; le glissement finit au relâchement, ou Échap le remet à sa place.
        void UpdateScreenDrag(Flux::Engine& engine, Flux::Entity selected, const Flux::UI::Rect& view);

        GizmoMode m_mode = GizmoMode::Move;
        int m_hoveredButton = -1;
        Drag m_drag;
        ScreenDrag m_screenDrag;
        int m_hoveredAxis = -1;
        bool m_modified = false;
        std::vector<Flux::RelativeTransform> m_transforms; // gardé d'une frame à l'autre : pas d'allocation
        // Éléments à l'écran dessinés à la frame précédente (sélection au clic).
        std::vector<Flux::ScreenUi::ItemRect> m_screenRects;
        // Ampoules des lumières et haut-parleurs des sons à l'écran, à cette frame (dessin et sélection au clic).
        struct LightIcon
        {
            Flux::Entity entity;
            Flux::Math::Vector2f center;
            Flux::UI::Icon icon;
        };
        std::vector<LightIcon> m_lightIcons;
    };
}
