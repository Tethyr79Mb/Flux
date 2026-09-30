#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/Vector2.hpp"

namespace Flux::UI {
    class DrawList;

    // Pictogrammes de l'éditeur, dessinés en formes simples (pas d'image) : nets à toutes les tailles, des lignes
    // d'arbre (14 pixels) aux vignettes (48 pixels).
    enum class Icon {
        None,
        Folder,
        File,
        Scene,       // fichier de scène : un clap
        Cube,
        Model,       // modèle 3D : un cube orange
        Character,
        Ship,
        Collider,    // boîte en fil de fer
        Seat,
        Text,
        Image,
        SceneChange, // flèche vers une autre scène
        Light,       // ampoule
        Component,   // composant du jeu, sans pictogramme à lui
        Empty,       // objet sans composant
    };

    // Dessine l'icône dans le carré [min, min + size].
    FLUX_API void DrawIcon(DrawList& list, Icon icon, Math::Vector2f min, float size);
}
