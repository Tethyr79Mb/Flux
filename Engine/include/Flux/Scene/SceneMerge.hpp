#pragma once

#include "Flux/Export.hpp"

#include <string>
#include <vector>

namespace Flux {
    class ComponentRegistry;
    class Scene;

    // Fusion à trois versions d'une scène, entités reconnues par leur identifiant : reprendre une partie sauvegardée
    // (`changed`, partie de `base`) dans la scène modifiée depuis dans l'éditeur (`target`, elle aussi partie de `base`).
    namespace SceneMerge {
        // Applique à target ce qui a changé de base à changed, champ par champ : entités déplacées, rattachées à un
        // autre parent, composants modifiés, ajoutés ou retirés, entités apparues ou détruites. Ce que target a changé
        // lui aussi (même champ) l'emporte, et une entité retirée de target n'y revient pas. Rend ce qui n'a pas pu
        // être repris (parent disparu...), une phrase par entité ; le reste de la fusion est fait quand même.
        FLUX_API std::vector<std::string> ApplyChanges(Scene& target, const Scene& base, const Scene& changed,
                                                       const ComponentRegistry& components);
    }
}
