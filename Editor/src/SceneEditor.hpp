#pragma once

#include <Flux/Core/Uuid.hpp>
#include <Flux/Math/Quaternion.hpp>
#include <Flux/Math/Vector3.hpp>
#include <Flux/Scene/Components.hpp>

#include <unordered_set>

namespace Flux {
    class Engine;
    class Scene;

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

        private:
        enum class Action { None, CreateRoot, CreateChild, Delete, RemoveMeshRenderer };

        void DrawHierarchy(Flux::Engine& engine, Flux::Scene& scene);
        void DrawNode(Flux::UI::Context& ui, Flux::Scene& scene, Flux::Entity entity);
        void DrawInspector(Flux::Engine& engine, Flux::Scene& scene, Flux::Entity selected);
        void ApplyAction(Flux::Engine& engine, Flux::Scene& scene, Flux::Entity selected);
        void Select(Flux::Scene& scene, Flux::Entity entity);

        Flux::Uuid m_selection;
        // Actions demandées pendant le dessin, appliquées après : on ne détruit rien en parcourant l'arbre.
        Action m_pendingAction = Action::None;
        // Ancêtres à déplier une fois, pour montrer une entité sélectionnée ou créée hors de l'arbre.
        std::unordered_set<Flux::Uuid> m_reveal;

        // Angles affichés pour la rotation : recalculés seulement si la rotation a changé ailleurs,
        // sinon un glissement franchissant la verticale ferait sauter les valeurs d'une frame à l'autre.
        Flux::Uuid m_anglesFor;
        Flux::Math::Quaternionf m_anglesSource;
        Flux::Math::Vector3f m_anglesDegrees;
    };
}
