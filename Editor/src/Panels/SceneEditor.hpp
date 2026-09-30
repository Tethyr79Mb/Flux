#pragma once

#include <Flux/Core/Uuid.hpp>
#include <Flux/Math/Quaternion.hpp>
#include <Flux/Math/Vector2.hpp>
#include <Flux/Math/Vector3.hpp>
#include <Flux/Scene/Components.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Flux {
    class Engine;
    class Scene;

    struct ComponentInfo;
    struct FieldInfo;

    namespace UI {
        class Context;
    }
}

namespace FluxEditor {
    class SceneDocument;
    class UndoHistory;

    // Panneaux « Scènes » et « Inspecteur » de l'éditeur, décrits à chaque frame avec FluxUI. L'onglet Scènes liste
    // les scènes du projet (un clic en ouvre une) ; sous la scène ouverte, ses objets racines rangés par type
    // (Vaisseaux, Personnages, Cubes...), chacun avec ses enfants en arbre.
    // La sélection est retenue par identifiant stable (Uuid) : elle survit au rechargement de la scène
    // et du module de jeu, qui recréent toutes les entités.
    class SceneEditor {
        public:
        // Les actions ponctuelles (créer, supprimer, ajouter un composant) sont enregistrées dans history.
        void Update(Flux::Engine& engine, UndoHistory& history, SceneDocument& document,
                    std::span<const std::filesystem::path> scenes);
        // Vrai si un champ de l'inspecteur a modifié l'entité sélectionnée depuis le dernier appel.
        bool TakeModified() { return std::exchange(m_modified, false); }

        // Entité sélectionnée (Uuid vide si aucune) ; Select(NullEntity) désélectionne.
        Flux::Uuid GetSelection() const { return m_selection; }
        // Déplie aussi ses ancêtres dans la hiérarchie, pour la montrer (sélection faite dans la vue 3D).
        void Select(Flux::Scene& scene, Flux::Entity entity);
        // Supprime l'entité sélectionnée (et ses enfants) au prochain Update : touche Suppr, menu Édition.
        // Sans effet sans sélection ou en mode Jouer.
        bool CanDelete() const { return m_selection && !m_readOnly; }
        void RequestDelete() {
            if (CanDelete()) {
                m_pendingAction = Action::Delete;
            }
        }

        // Le personnage sélectionné devient le seul « dirigé par le joueur » (les autres sont décochés, annulable).
        void RequestClaimPlayer() {
            if (m_selection && !m_readOnly) {
                m_pendingAction = Action::ClaimPlayer;
            }
        }

        private:
        enum class Action { None, CreateRoot, CreateChild, Delete, AddComponent, RemoveComponent, Move, ClaimPlayer };

        // Où poser l'entité glissée dans la hiérarchie, par rapport à la ligne survolée.
        enum class DropZone { Before, After, Inside };

        // Glisser-déposer dans la hiérarchie : change l'ordre (donc l'ordre d'affichage de l'interface à l'écran)
        // ou le parent.
        struct HierarchyDrag
        {
            Flux::Uuid entity;          // ligne tenue (vide : rien)
            Flux::Math::Vector2f pressMouse;
            bool dragging = false;      // la souris s'est éloignée : c'est un glisser-déposer, pas un clic
            Flux::Uuid target;          // ligne survolée cette frame (vide : aucune cible valable)
            DropZone zone = DropZone::Inside;
        };

        void DrawScenes(Flux::Engine& engine, Flux::Scene& scene, SceneDocument& document,
                        std::span<const std::filesystem::path> scenes);
        void DrawObjects(Flux::UI::Context& ui, Flux::Engine& engine, Flux::Scene& scene);
        void DrawNode(Flux::UI::Context& ui, Flux::Scene& scene, Flux::Entity entity);
        void DrawInspector(Flux::Engine& engine, Flux::Scene& scene, Flux::Entity selected);
        void DrawComponent(Flux::Engine& engine, const Flux::ComponentInfo& info, void* component);
        bool DrawField(Flux::Engine& engine, const Flux::ComponentInfo& info, const Flux::FieldInfo& field, void* value);
        bool DrawAnimationChoice(Flux::Engine& engine, const std::string& label, std::string& text);
        void ApplyAction(Flux::Engine& engine, Flux::Scene& scene, Flux::Entity selected, UndoHistory& history);

        Flux::Uuid m_selection;
        bool m_modified = false;
        bool m_readOnly = false; // mode Jouer : on regarde, on ne modifie pas
        Flux::Entity m_inspected = Flux::NullEntity; // entité dont l'inspecteur dessine les champs
        // Actions demandées pendant le dessin, appliquées après : on ne détruit rien en parcourant l'arbre.
        Action m_pendingAction = Action::None;
        std::string m_pendingComponent; // clé du composant à ajouter ou retirer
        HierarchyDrag m_hierarchyDrag;
        HierarchyDrag m_pendingMove;       // dépôt à appliquer (Action::Move)
        Flux::Math::Vector2f m_mouse;      // souris de la frame, pour le glisser-déposer
        // Ancêtres à déplier une fois, pour montrer une entité sélectionnée ou créée hors de l'arbre.
        std::unordered_set<Flux::Uuid> m_reveal;
        Flux::Uuid m_revealRoot; // racine dont le groupe est à déplier une fois
        std::filesystem::path m_shownScene; // scène dépliée dans l'onglet (la déplier encore quand elle change)

        // Angles affichés pour les rotations de l'entité inspectée (clé : "composant/champ") : recalculés seulement
        // si la rotation a changé ailleurs, sinon un glissement franchissant la verticale ferait sauter les valeurs.
        struct Angles
        {
            Flux::Math::Quaternionf source;
            Flux::Math::Vector3f degrees;
        };
        Flux::Uuid m_anglesFor;
        std::unordered_map<std::string, Angles> m_angles;
    };
}
