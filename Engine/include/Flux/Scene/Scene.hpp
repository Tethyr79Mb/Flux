#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/WorldPosition.hpp"
#include "Flux/Scene/Components.hpp"

#include <entt/entity/registry.hpp>

#include <string_view>
#include <utility>
#include <vector>

namespace Flux {
    // Transformation d'une entité exprimée par rapport à un point de référence (la caméra) : de petits nombres,
    // prêts à passer en float pour le GPU, même à l'autre bout de la galaxie.
    struct RelativeTransform
    {
        Entity entity = NullEntity;
        Math::Vector3d position;
        Math::Quaternionf rotation;
        Math::Vector3f scale{1.0f, 1.0f, 1.0f};
    };

    // Ensemble d'entités (EnTT) organisées en hiérarchie. Toute entité a un nom, une transformation et des liens de hiérarchie.
    class FLUX_API Scene {
        public:
        Scene() = default;
        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;

        Entity CreateEntity(std::string_view name, Entity parent = NullEntity);
        // Détruit aussi tous les descendants.
        void DestroyEntity(Entity entity);
        void Clear();
        bool IsValid(Entity entity) const { return m_registry.valid(entity); }

        // parent = NullEntity pour en faire une racine. La transformation locale est conservée telle quelle
        // (l'entité peut donc se déplacer à l'écran), et le secteur est perdu en devenant enfant.
        // Refuse (retourne false) de rendre une entité enfant d'elle-même ou d'un de ses descendants.
        bool SetParent(Entity entity, Entity parent);
        Entity GetParent(Entity entity) const { return m_registry.get<HierarchyComponent>(entity).parent; }

        // Position absolue (secteur + décalage), en composant toute la hiérarchie.
        Math::WorldPosition GetWorldPosition(Entity entity) const;

        // Transformations de toutes les entités, relatives à origin (parents avant enfants).
        void ComputeRelativeTransforms(const Math::WorldPosition& origin, std::vector<RelativeTransform>& out) const;

        template <typename Function>
        void ForEachChild(Entity entity, Function&& function) const {
            for (Entity child = m_registry.get<HierarchyComponent>(entity).firstChild; child != NullEntity;) {
                const Entity next = m_registry.get<HierarchyComponent>(child).nextSibling; // l'appel peut détruire child
                function(child);
                child = next;
            }
        }

        template <typename Function>
        void ForEachRoot(Function&& function) const {
            for (const auto [entity, hierarchy] : m_registry.view<const HierarchyComponent>().each()) {
                if (hierarchy.parent == NullEntity) {
                    function(entity);
                }
            }
        }

        template <typename Component, typename... Args>
        Component& Add(Entity entity, Args&&... args) {
            return m_registry.emplace_or_replace<Component>(entity, std::forward<Args>(args)...);
        }

        template <typename Component>
        Component& Get(Entity entity) { return m_registry.get<Component>(entity); }
        template <typename Component>
        const Component& Get(Entity entity) const { return m_registry.get<Component>(entity); }

        template <typename Component>
        Component* TryGet(Entity entity) { return m_registry.try_get<Component>(entity); }
        template <typename Component>
        const Component* TryGet(Entity entity) const { return m_registry.try_get<Component>(entity); }

        // Accès direct pour les systèmes (vues sur plusieurs composants). Ne pas toucher à HierarchyComponent.
        entt::registry& GetRegistry() { return m_registry; }
        const entt::registry& GetRegistry() const { return m_registry; }

        private:
        void Detach(Entity entity);
        void AppendChild(Entity parent, Entity child);
        void ComputeSubtree(Entity entity, const RelativeTransform& parent, std::vector<RelativeTransform>& out) const;

        entt::registry m_registry;
    };
}
