#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/WorldPosition.hpp"
#include "Flux/Scene/Components.hpp"

#include <entt/entity/registry.hpp>

#include <string_view>
#include <type_traits>
#include <unordered_map>
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

        // id vide : un nouvel identifiant est tiré au hasard (le chargement de scène fournit l'identifiant sauvegardé).
        Entity CreateEntity(std::string_view name, Entity parent = NullEntity, Uuid id = {});
        // Détruit aussi tous les descendants.
        void DestroyEntity(Entity entity);
        // Supprime toutes les entités et tous les stockages de composants.
        void Clear();
        bool IsValid(Entity entity) const { return m_registry.valid(entity); }

        Uuid GetId(Entity entity) const { return m_registry.get<IdComponent>(entity).id; }
        // NullEntity si aucune entité ne porte cet identifiant.
        Entity FindById(Uuid id) const;
        // Une entité portant ce nom, NullEntity sinon (recherche linéaire : à éviter à chaque frame).
        Entity FindByName(std::string_view name) const;
        std::size_t GetEntityCount() const { return m_entitiesById.size(); }

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

        // Dans l'ordre de création (ou de rattachement) : stable d'une sauvegarde à l'autre.
        template <typename Function>
        void ForEachRoot(Function&& function) const {
            for (Entity root = m_firstRoot; root != NullEntity;) {
                const Entity next = m_registry.get<HierarchyComponent>(root).nextSibling; // l'appel peut détruire root
                function(root);
                root = next;
            }
        }

        template <typename Component, typename... Args>
        Component& Add(Entity entity, Args&&... args) {
            return m_registry.emplace_or_replace<Component>(entity, std::forward<Args>(args)...);
        }

        // Retire un composant facultatif (MeshRendererComponent...). Jamais Id, Name, Transform ni Hierarchy.
        template <typename Component>
        void Remove(Entity entity) {
            static_assert(!std::is_same_v<Component, IdComponent> && !std::is_same_v<Component, NameComponent>
                          && !std::is_same_v<Component, TransformComponent> && !std::is_same_v<Component, HierarchyComponent>,
                          "composant obligatoire");
            m_registry.remove<Component>(entity);
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
        std::unordered_map<Uuid, Entity> m_entitiesById;
        // Les racines forment une liste de frères, comme des enfants sans parent.
        Entity m_firstRoot = NullEntity;
        Entity m_lastRoot = NullEntity;
    };
}
