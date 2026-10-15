#pragma once

#include <Flux/Scene/Scene.hpp>

#include <string>

namespace FluxEditor {
    // Aimant des armes : une arme (WeaponComponent) lâchée sur un emport (HardpointComponent) dans l'onglet Scènes,
    // ou déplacée au gizmo près de lui, s'y fixe : elle devient son enfant, à sa place exacte.
    namespace Hardpoints {
        // Distance (m) entre l'arme lâchée au gizmo et l'emport sous laquelle elle s'y fixe.
        inline constexpr double SnapDistance = 1.0;

        // Emport le plus proche de l'arme, à moins de SnapDistance (son emport actuel compris) ; NullEntity sinon.
        Flux::Entity FindNearby(const Flux::Scene& scene, Flux::Entity weapon);
        // Pourquoi l'emport refuse l'arme : autre genre, trop grande, ou il en porte déjà une ; vide s'il l'accepte.
        std::string GetRefusal(const Flux::Scene& scene, Flux::Entity weapon, Flux::Entity hardpoint);
        // Fixe l'arme sur l'emport : enfant, position et rotation à zéro (échelle gardée). false si la hiérarchie
        // refuse (l'emport est dans l'arme).
        bool Mount(Flux::Scene& scene, Flux::Entity weapon, Flux::Entity hardpoint);
    }
}
