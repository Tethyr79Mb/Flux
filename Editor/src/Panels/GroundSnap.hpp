#pragma once

#include <Flux/Math/Vector3.hpp>
#include <Flux/Math/WorldPosition.hpp>
#include <Flux/Scene/Picking.hpp>
#include <Flux/Scene/Scene.hpp>

#include <optional>
#include <span>
#include <vector>

namespace Flux {
    class Engine;
}

namespace FluxEditor {
    // Pose au sol dans l'éditeur (bouton « Poser au sol » de la vue 3D, touche Fin, Placer) : le bas d'un objet,
    // descendants compris, vient sur le dessus de ce qui est dessous, ou sur le plan de la grille (y = 0) dans une
    // scène sans planète. Les boîtes sont celles de la sélection au clic (Engine::GetMeshBounds) : le dessus d'un
    // modèle est celui de sa boîte, pas de sa forme.
    struct GroundProbe
    {
        std::vector<Flux::RelativeTransform> parts; // l'objet et ses descendants
        std::vector<Flux::GroundBox> boxes;         // le reste de la scène
        Flux::Extent extent;
        std::optional<double> planeY;
    };

    // Prépare la recherche pour entity, placée en position ; transforms relatifs à origin (Scene::ComputeRelativeTransforms).
    // false si l'objet n'a pas de boîte (lumière, son, ciel, planète...) : il reste où il est.
    bool PrepareGround(Flux::Engine& engine, Flux::Entity entity, const Flux::Math::WorldPosition& origin,
                       std::span<const Flux::RelativeTransform> transforms, const Flux::Math::Vector3d& position,
                       GroundProbe& probe);

    // Hauteur à donner à l'origine de l'objet placé en position (même espace que la préparation), les rayons partant
    // de from (au moins le haut de l'objet) ; nullopt si rien n'est dessous.
    std::optional<double> FindGround(const GroundProbe& probe, const Flux::Math::Vector3d& position,
                                     std::optional<double> from = std::nullopt);
}
