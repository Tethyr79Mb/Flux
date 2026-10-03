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
        Sky,         // ciel étoilé : un disque bleu nuit, une lune et des étoiles
        Component,   // composant du jeu, sans pictogramme à lui
        Empty,       // objet sans composant
        Window,      // fenêtre d'interface (aussi un fichier .fluxui) : barre de titre et fond
        Frame,       // cadre d'interface
        Button,
        Checkbox,
        Slider,
        // Commandes et panneaux de l'éditeur.
        Play,      // triangle vert
        Pause,     // deux barres
        Stop,      // carré rouge
        Plus,
        Camera,
        Inspector, // trois réglettes
        Place,     // un cube et un plus : poser des objets
        Content,   // dossier ouvert : fichiers du projet
        Search,    // loupe
        Console,   // terminal : « >_ »
        // Messages de la console : information, avertissement, erreur.
        Info,      // disque bleu, « i »
        Warning,   // triangle jaune, « ! »
        Error,     // disque rouge, croix
    };

    // Dessine l'icône dans le carré [min, min + size].
    FLUX_API void DrawIcon(DrawList& list, Icon icon, Math::Vector2f min, float size);
}
