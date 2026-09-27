#pragma once

#include "Flux/Core/Uuid.hpp"
#include "Flux/Math/Color.hpp"
#include "Flux/Math/Quaternion.hpp"
#include "Flux/Math/Vector3.hpp"

#include <entt/entity/entity.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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

    // Point de l'écran (ou de la vue de l'éditeur) auquel un élément d'interface est accroché.
    enum class ScreenAnchor : std::uint8_t {
        TopLeft,
        Top,
        TopRight,
        Left,
        Center,
        Right,
        BottomLeft,
        Bottom,
        BottomRight,
    };

    // Texte collé à l'écran (titre, menu), dessiné par-dessus la 3D. La transformation de l'entité est ignorée.
    // Taille et décalage sont en pixels d'un écran de 1080 lignes (ScreenUi::ReferenceHeight) : le texte garde
    // la même place dans toutes les résolutions, et dans la vue de l'éditeur.
    struct ScreenTextComponent
    {
        std::string text = "Texte";
        float size = 64.0f;
        Math::LinearColor color{1.0f, 1.0f, 1.0f};
        ScreenAnchor anchor = ScreenAnchor::Center; // aussi le point du texte placé sur l'ancre (centre sur centre...)
        float offsetX = 0.0f; // vers la droite
        float offsetY = 0.0f; // vers le bas
    };

    // Composants d'un fichier de scène dont le type n'est pas enregistré (module de jeu absent ou ancien) :
    // gardés tels quels (clé, texte JSON) et réécrits à la sauvegarde, pour ne jamais perdre de données.
    struct UnknownComponentsComponent
    {
        std::vector<std::pair<std::string, std::string>> components;
    };
}
