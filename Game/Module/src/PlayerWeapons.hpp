#pragma once

namespace Flux {
    class Engine;
}

// Armes du joueur : touches des groupes (1 à 0), verrouillage de la cible au centre de la vue, tir automatique, et
// leur affichage (coins autour de la cible, groupes actifs, missiles). Les armes sont au jeu (Weapons.cpp) ; le moteur
// fait voler les projectiles. Ici : quand et sur quoi le joueur tire.
namespace Game {
    void RegisterPlayerWeapons(Flux::Engine& engine);
    void UnregisterPlayerWeapons(Flux::Engine& engine);
}
