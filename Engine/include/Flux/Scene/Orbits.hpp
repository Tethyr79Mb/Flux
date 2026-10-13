#pragma once

#include "Flux/Export.hpp"

namespace Flux {
    class Scene;

    namespace Orbits {
        // Place chaque objet qui a une Orbite (OrbitComponent) autour du Soleil de la scène (le premier SunComponent).
        // Une orbite de distance nulle prend d'abord ses valeurs de la position actuelle : l'objet ne bouge pas quand
        // on ajoute le composant. Sans Soleil, rien ne bouge.
        FLUX_API void Update(Scene& scene);
    }
}
