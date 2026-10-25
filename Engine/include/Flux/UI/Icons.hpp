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
        Prefab,      // prefab (fichier ou exemplaire dans la scène) : un cube bleu vif
        Character,
        Ship,
        Collider,    // boîte en fil de fer
        Seat,
        Text,
        Image,
        SceneChange, // flèche vers une autre scène
        Light,       // ampoule
        Sound,       // haut-parleur et ondes
        Sky,         // ciel étoilé : un disque bleu nuit, une lune et des étoiles
        Planet,      // planète ocre avec un anneau
        Component,   // composant du jeu, sans pictogramme à lui
        Empty,       // objet sans composant
        Window,      // fenêtre d'interface (aussi un fichier .fluxui) : barre de titre et fond
        Frame,       // cadre d'interface
        Button,
        Checkbox,
        Slider,
        ProgressBar, // barre de progression d'interface, remplie aux deux tiers
        Tabs,        // onglets d'interface : trois onglets, le premier choisi, sur une page
        ScrollArea,  // zone de défilement : des lignes et un ascenseur à droite
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
        Save,      // disquette
        Undo,      // flèche courbe vers la gauche
        Redo,      // flèche courbe vers la droite
        Move,      // quatre flèches : déplacer
        Rotate,    // cercle fléché : tourner
        Scale,     // petit carré dans un grand, flèche en diagonale : mettre à l'échelle
        Settings,  // roue dentée
        Up,        // flèche vers le haut : dossier parent
        Grid,      // grille au sol, vue en perspective
        DropToGround, // flèche vers le bas posée sur un trait de sol : poser au sol
        // Messages de la console : information, avertissement, erreur.
        Info,      // disque bleu, « i »
        Warning,   // triangle jaune, « ! »
        Error,     // disque rouge, croix
        // Ajoutés ensuite (à la fin : les valeurs d'avant ne changent pas pour le module du jeu).
        Tree,      // sapin : espèce d'arbre (fichier .espece.json)
        Biome,     // sapin et feuillu sur une colline : biome (fichier .biome.json)
    };

    // Dessine l'icône dans le carré [min, min + size].
    FLUX_API void DrawIcon(DrawList& list, Icon icon, Math::Vector2f min, float size);
}
