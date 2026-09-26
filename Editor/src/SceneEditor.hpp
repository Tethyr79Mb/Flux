#pragma once

#include <Flux/Core/Uuid.hpp>
#include <Flux/Math/Quaternion.hpp>
#include <Flux/Math/Vector3.hpp>
#include <Flux/Scene/Components.hpp>

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
    // Panneaux « Hiérarchie » et « Inspecteur » de l'éditeur, décrits à chaque frame avec FluxUI.
    // La sélection est retenue par identifiant stable (Uuid) : elle survit au rechargement de la scène
    // et du module de jeu, qui recréent toutes les entités.
    class SceneEditor {
        public:
        void Update(Flux::Engine& engine);
        // Vrai si la scène a été modifiée depuis le dernier appel (pour l'étoile du titre).
        bool TakeModified() { return std::exchange(m_modified, false); }

        private:
        enum class Action { None, CreateRoot, CreateChild, Delete, AddComponent, RemoveComponent };

        void DrawHierarchy(Flux::Engine& engine, Flux::Scene& scene);
        void DrawNode(Flux::UI::Context& ui, Flux::Scene& scene, Flux::Entity entity);
        void DrawInspector(Flux::Engine& engine, Flux::Scene& scene, Flux::Entity selected);
        void DrawComponent(Flux::UI::Context& ui, const Flux::ComponentInfo& info, void* component);
        bool DrawField(Flux::UI::Context& ui, const Flux::ComponentInfo& info, const Flux::FieldInfo& field, void* value);
        void ApplyAction(Flux::Engine& engine, Flux::Scene& scene, Flux::Entity selected);
        void Select(Flux::Scene& scene, Flux::Entity entity);

        Flux::Uuid m_selection;
        bool m_modified = false;
        // Actions demandées pendant le dessin, appliquées après : on ne détruit rien en parcourant l'arbre.
        Action m_pendingAction = Action::None;
        std::string m_pendingComponent; // clé du composant à ajouter ou retirer
        // Ancêtres à déplier une fois, pour montrer une entité sélectionnée ou créée hors de l'arbre.
        std::unordered_set<Flux::Uuid> m_reveal;

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
