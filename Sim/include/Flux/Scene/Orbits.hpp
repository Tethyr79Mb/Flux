#pragma once

#include "Flux/Scene/Components.hpp"
#include "Flux/SimExport.hpp"

namespace Flux {
    class Scene;

    namespace Orbits {
        // Place chaque objet qui a une Orbite (OrbitComponent) autour du Soleil de la scène (le premier SunComponent),
        // ou autour de son parent s'il en a un (une lune). Une orbite de distance nulle prend d'abord ses valeurs de la
        // position actuelle : l'objet ne bouge pas quand on ajoute le composant. Sans Soleil, seules les lunes bougent.
        FLUX_SIM_API void Update(Scene& scene);
        // Après un changement de parent (glisser dans la hiérarchie) : une planète lâchée sur une planète devient sa
        // lune, à MoonDistance rayons d'elle (la Lune est à 60 rayons de la Terre) ; toute autre orbite est reprise
        // de la position, qui ne bouge pas.
        FLUX_SIM_API void Reparented(Scene& scene, Entity entity);
        inline constexpr double MoonDistance = 60.0;
    }
}
