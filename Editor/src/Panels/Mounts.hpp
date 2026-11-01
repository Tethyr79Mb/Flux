#pragma once

#include <Flux/Core/EditorExtensions.hpp>
#include <Flux/Scene/Scene.hpp>

namespace FluxEditor {
    // Aimant (MountRule, déclaré par le jeu) : une pièce (une arme) lâchée sur un support (un emport) dans l'onglet
    // Scènes, ou déplacée au gizmo près de lui, s'y fixe : elle devient son enfant, à sa place exacte.
    namespace Mounts {
        // Distance (m) entre la pièce lâchée au gizmo et le support sous laquelle elle s'y fixe.
        inline constexpr double SnapDistance = 1.0;

        // Support de la règle le plus proche de la pièce, à moins de SnapDistance (son support actuel compris) ;
        // NullEntity sinon.
        Flux::Entity FindNearby(const Flux::Scene& scene, const Flux::MountRule& rule, Flux::Entity part);
        // Fixe la pièce sur le support : enfant, position et rotation à zéro (échelle gardée). false si la hiérarchie
        // refuse (le support est dans la pièce).
        bool Mount(Flux::Scene& scene, Flux::Entity part, Flux::Entity mount);
    }
}
