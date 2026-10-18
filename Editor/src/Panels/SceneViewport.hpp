#pragma once

#include "Panels/GroundSnap.hpp"

#include <Flux/Core/Uuid.hpp>
#include <Flux/Math/Quaternion.hpp>
#include <Flux/Math/Vector3.hpp>
#include <Flux/Math/WorldPosition.hpp>
#include <Flux/Scene/Picking.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/ScreenUi.hpp>
#include <Flux/UI/Context.hpp>

#include <array>
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
    // vue ou les touches W, E, R ; à côté, la grille au sol s'affiche ou se cache (touche G), puis l'aimantation comme
    // dans Unreal (déplacement sur la grille, rotation et échelle par pas, chacune avec son pas ; Ctrl tenu l'inverse
    // le temps d'un glissement) et la pose au sol (un objet déplacé à l'horizontale ou posé depuis Placer descend sur
    // ce qui est dessous ; touche Fin : poser la sélection tout de suite). Les textes et images à
    // l'écran de la scène y sont dessinés comme dans le jeu, et se déplacent en les glissant. Les lumières y sont montrées par une ampoule (sélection au clic) et, sélectionnées,
    // par leur portée.
    class SceneViewport {
        public:
        enum class GizmoMode { Move, Rotate, Scale };

        // view : rectangle de la vue 3D (retourné par EditorLayout::Update).
        void Update(Flux::Engine& engine, SceneEditor& sceneEditor, const Flux::UI::Rect& view);
        // Bouton « Poser au sol » enfoncé : Placer y pose aussi les objets.
        bool IsDroppingToGround() const { return m_dropToGround; }
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
            float applied = 0.0f;                     // rotation : angle appliqué (par pas si l'aimantation est active)
            bool ground = false;                      // déplacement horizontal posé au sol (m_ground préparé)
        };

        // Aimantation d'un mode du gizmo : active ou non, et l'indice de son pas (SnapSteps dans le .cpp).
        struct Snap
        {
            bool enabled = true;
            int step = 0;
        };

        void HandleShortcuts(Flux::Engine& engine);
        bool WantsFocus(Flux::Engine& engine) const;
        static void FocusOn(Flux::Engine& engine, const Flux::Scene& scene, Flux::Entity entity);
        // Boutons de mode, de la grille, de l'aimantation et de la pose au sol en haut à gauche de la vue (et la
        // liste ouverte d'un pas) ; retourne true si la souris en a cliqué un.
        bool UpdateToolbar(Flux::Engine& engine, const Flux::UI::Rect& view, bool mouseInView);
        void DrawToolbar(Flux::Engine& engine, const Flux::UI::Rect& view);
        // Applique le glissement en cours à la transformation locale de l'entité.
        void UpdateDrag(Flux::Engine& engine, Flux::Entity selected, Flux::RelativeTransform& selectedTransform,
                        const Flux::RelativeTransform* parentTransform, const Flux::Ray& ray);
        // Aimantation de ce mode pour ce glissement : celle des boutons, inversée tant que Ctrl est tenu.
        bool IsSnapping(Flux::Engine& engine, GizmoMode mode) const;
        double GetSnapStep(GizmoMode mode) const;
        // Touche Fin : la sélection descend sur ce qui est dessous.
        void DropToGround(Flux::Engine& engine, Flux::Entity selected, const Flux::RelativeTransform& selectedTransform,
                          const Flux::RelativeTransform* parentTransform);
        // Arme lâchée au gizmo près d'un emport : s'y fixe s'il l'accepte (Hardpoints).
        void SnapToHardpoint(Flux::Scene& scene, Flux::Entity weapon);

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
        bool m_grid = true;       // grille au sol (bouton à droite des modes, touche G)
        // Déplacement 10 cm, rotation 10°, échelle 0,25, comme les réglages de départ d'Unreal.
        std::array<Snap, 3> m_snaps{{{true, 2}, {true, 1}, {true, 2}}};
        bool m_dropToGround = true;
        int m_openSnapList = -1; // mode dont la liste des pas est ouverte, -1 : aucune
        int m_hoveredButton = -1;
        int m_hoveredStep = -1;  // pas survolé dans la liste ouverte
        GroundProbe m_ground;    // boîtes de la scène au départ d'un déplacement posé au sol
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
