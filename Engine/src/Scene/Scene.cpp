#include "Flux/Scene/Scene.hpp"

#include <string>

namespace Flux {
    Entity Scene::CreateEntity(std::string_view name, Entity parent) {
        const Entity entity = m_registry.create();
        m_registry.emplace<NameComponent>(entity, std::string(name));
        m_registry.emplace<TransformComponent>(entity);
        m_registry.emplace<HierarchyComponent>(entity);
        if (parent != NullEntity) {
            AppendChild(parent, entity);
        }
        return entity;
    }

    void Scene::DestroyEntity(Entity entity) {
        if (!IsValid(entity)) {
            return;
        }
        ForEachChild(entity, [this](Entity child) { DestroyEntity(child); });
        Detach(entity);
        m_registry.destroy(entity);
    }

    void Scene::Clear() {
        m_registry.clear();
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
            AppendChild(parent, entity);
        }
        return true;
    }

    void Scene::Detach(Entity entity) {
        auto& hierarchy = m_registry.get<HierarchyComponent>(entity);
        if (hierarchy.parent == NullEntity) {
            return;
        }
        if (hierarchy.previousSibling != NullEntity) {
            m_registry.get<HierarchyComponent>(hierarchy.previousSibling).nextSibling = hierarchy.nextSibling;
        } else {
            m_registry.get<HierarchyComponent>(hierarchy.parent).firstChild = hierarchy.nextSibling;
        }
        if (hierarchy.nextSibling != NullEntity) {
            m_registry.get<HierarchyComponent>(hierarchy.nextSibling).previousSibling = hierarchy.previousSibling;
        }
        hierarchy.parent = NullEntity;
        hierarchy.nextSibling = NullEntity;
        hierarchy.previousSibling = NullEntity;
    }

    void Scene::AppendChild(Entity parent, Entity child) {
        auto& childHierarchy = m_registry.get<HierarchyComponent>(child);
        childHierarchy.parent = parent;

        // Ajout en fin de liste : l'ordre de création est conservé (panneau Hiérarchie de l'éditeur).
        auto& parentHierarchy = m_registry.get<HierarchyComponent>(parent);
        if (parentHierarchy.firstChild == NullEntity) {
            parentHierarchy.firstChild = child;
            return;
        }
        Entity last = parentHierarchy.firstChild;
        while (m_registry.get<HierarchyComponent>(last).nextSibling != NullEntity) {
            last = m_registry.get<HierarchyComponent>(last).nextSibling;
        }
        m_registry.get<HierarchyComponent>(last).nextSibling = child;
        childHierarchy.previousSibling = last;
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
