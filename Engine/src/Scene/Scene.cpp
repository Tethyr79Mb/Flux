#include "Flux/Scene/Scene.hpp"

#include <string>

namespace Flux {
    Entity Scene::CreateEntity(std::string_view name, Entity parent, Uuid id) {
        // Un identifiant déjà pris (fichier copié-collé à la main, par exemple) est remplacé plutôt que partagé.
        while (!id || m_entitiesById.contains(id)) {
            id = Uuid::Generate();
        }
        const Entity entity = m_registry.create();
        m_entitiesById.emplace(id, entity);
        m_registry.emplace<IdComponent>(entity, id);
        m_registry.emplace<NameComponent>(entity, std::string(name));
        m_registry.emplace<TransformComponent>(entity);
        m_registry.emplace<HierarchyComponent>(entity);
        AppendChild(parent, entity);
        return entity;
    }

    void Scene::DestroyEntity(Entity entity) {
        if (!IsValid(entity)) {
            return;
        }
        ForEachChild(entity, [this](Entity child) { DestroyEntity(child); });
        Detach(entity);
        m_entitiesById.erase(GetId(entity));
        m_registry.destroy(entity);
    }

    void Scene::Clear() {
        m_registry.clear();
        m_entitiesById.clear();
        m_firstRoot = NullEntity;
        m_lastRoot = NullEntity;
    }

    Entity Scene::FindById(Uuid id) const {
        const auto found = m_entitiesById.find(id);
        return found != m_entitiesById.end() ? found->second : NullEntity;
    }

    Entity Scene::FindByName(std::string_view name) const {
        for (const auto [entity, component] : m_registry.view<const NameComponent>().each()) {
            if (component.name == name) {
                return entity;
            }
        }
        return NullEntity;
    }

    bool Scene::SetParent(Entity entity, Entity parent) {
        for (Entity ancestor = parent; ancestor != NullEntity; ancestor = GetParent(ancestor)) {
            if (ancestor == entity) {
                return false;
            }
        }
        Detach(entity);
        if (parent != NullEntity) {
            m_registry.remove<SectorComponent>(entity);
        }
        AppendChild(parent, entity);
        return true;
    }

    void Scene::Detach(Entity entity) {
        auto& hierarchy = m_registry.get<HierarchyComponent>(entity);
        if (hierarchy.previousSibling != NullEntity) {
            m_registry.get<HierarchyComponent>(hierarchy.previousSibling).nextSibling = hierarchy.nextSibling;
        } else if (hierarchy.parent != NullEntity) {
            m_registry.get<HierarchyComponent>(hierarchy.parent).firstChild = hierarchy.nextSibling;
        } else {
            m_firstRoot = hierarchy.nextSibling;
        }
        if (hierarchy.nextSibling != NullEntity) {
            m_registry.get<HierarchyComponent>(hierarchy.nextSibling).previousSibling = hierarchy.previousSibling;
        } else if (hierarchy.parent != NullEntity) {
            m_registry.get<HierarchyComponent>(hierarchy.parent).lastChild = hierarchy.previousSibling;
        } else {
            m_lastRoot = hierarchy.previousSibling;
        }
        hierarchy.parent = NullEntity;
        hierarchy.nextSibling = NullEntity;
        hierarchy.previousSibling = NullEntity;
    }

    void Scene::AppendChild(Entity parent, Entity child) {
        auto& childHierarchy = m_registry.get<HierarchyComponent>(child);
        childHierarchy.parent = parent;

        // Ajout en fin de liste : l'ordre de création est conservé (panneau Hiérarchie de l'éditeur).
        auto* parentHierarchy = parent != NullEntity ? &m_registry.get<HierarchyComponent>(parent) : nullptr;
        Entity& first = parentHierarchy ? parentHierarchy->firstChild : m_firstRoot;
        Entity& last = parentHierarchy ? parentHierarchy->lastChild : m_lastRoot;
        if (last == NullEntity) {
            first = child;
        } else {
            m_registry.get<HierarchyComponent>(last).nextSibling = child;
            childHierarchy.previousSibling = last;
        }
        last = child;
    }

    Math::WorldPosition Scene::GetWorldPosition(Entity entity) const {
        // Remonte jusqu'à la racine en accumulant la position dans le repère de chaque parent.
        Math::Vector3d position = m_registry.get<TransformComponent>(entity).position;
        Entity current = entity;
        for (Entity parent = GetParent(current); parent != NullEntity; current = parent, parent = GetParent(current)) {
            const auto& transform = m_registry.get<TransformComponent>(parent);
            const Math::Vector3d scaled{position.x * transform.scale.x, position.y * transform.scale.y, position.z * transform.scale.z};
            position = transform.position + transform.rotation.Rotate(scaled);
        }
        Math::WorldPosition world;
        if (const auto* sector = m_registry.try_get<SectorComponent>(current)) {
            world.sector = sector->sector;
        }
        world += position;
        return world;
    }

    void Scene::ComputeRelativeTransforms(const Math::WorldPosition& origin, std::vector<RelativeTransform>& out) const {
        out.clear();
        ForEachRoot([&](Entity root) {
            const auto& transform = m_registry.get<TransformComponent>(root);
            Math::WorldPosition rootPosition;
            if (const auto* sector = m_registry.try_get<SectorComponent>(root)) {
                rootPosition.sector = sector->sector;
            }
            rootPosition.offset = transform.position; // non normalisé : Delta s'en accommode
            const RelativeTransform relative{root, Math::Delta(origin, rootPosition), transform.rotation, transform.scale};
            out.push_back(relative);
            ForEachChild(root, [&](Entity child) { ComputeSubtree(child, relative, out); });
        });
    }

    void Scene::ComputeSubtree(Entity entity, const RelativeTransform& parent, std::vector<RelativeTransform>& out) const {
        const auto& local = m_registry.get<TransformComponent>(entity);
        const Math::Vector3d scaled{local.position.x * parent.scale.x, local.position.y * parent.scale.y, local.position.z * parent.scale.z};
        // Échelle composée composante par composante : exact tant que les échelles non uniformes ne sont pas tournées.
        const RelativeTransform relative{
            entity,
            parent.position + parent.rotation.Rotate(scaled),
            Math::Normalize(parent.rotation * local.rotation),
            {parent.scale.x * local.scale.x, parent.scale.y * local.scale.y, parent.scale.z * local.scale.z},
        };
        out.push_back(relative);
        ForEachChild(entity, [&](Entity child) { ComputeSubtree(child, relative, out); });
    }
}
