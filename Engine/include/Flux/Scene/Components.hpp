#pragma once

#include "Flux/Core/Uuid.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Quaternion.hpp"
#include "Flux/Math/Vector3.hpp"

#include <entt/entity/entity.hpp>

#include <cstdint>
#include <string>

namespace Flux {
    using Entity = entt::entity;
    inline constexpr Entity NullEntity = entt::null;

    // Identifiant stable, sauvegardé : sert aux références entre entités (parent, cibles, undo).
    struct IdComponent
    {
        Uuid id;
    };

    struct NameComponent
    {
        std::string name;
    };

    // Transformation locale : relative au parent, ou au secteur pour une racine.
    // La position est en double pour rester précise jusqu'aux bords d'un secteur (1 million de km).
    struct TransformComponent
    {
        Math::Vector3d position;
        Math::Quaternionf rotation;
        Math::Vector3f scale{1.0f, 1.0f, 1.0f};
    };

    // Secteur d'une entité racine (absent : secteur 0). Voir Math::WorldPosition.
    struct SectorComponent
    {
        Math::Vector3i64 sector;
    };

    // Liens de la hiérarchie, en liste chaînée : aucune allocation par enfant. Les racines sont aussi chaînées entre elles.
    // Géré par Scene uniquement (CreateEntity, SetParent, DestroyEntity).
    struct HierarchyComponent
    {
        Entity parent = NullEntity;
        Entity firstChild = NullEntity;
        Entity lastChild = NullEntity; // ajout en fin de liste sans la parcourir
        Entity nextSibling = NullEntity;
        Entity previousSibling = NullEntity;
    };

    enum class MeshType : std::uint8_t {
        Cube, // cube unité centré
    };

    struct MeshRendererComponent
    {
        MeshType mesh = MeshType::Cube;
        Math::LinearColor color{1.0f, 1.0f, 1.0f};
    };
}
