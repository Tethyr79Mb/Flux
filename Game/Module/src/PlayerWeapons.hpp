#pragma once

namespace Flux {
    class Engine;
}

// Armes du joueur : touches des groupes (1 à 0), verrouillage de la cible au centre de la vue, tir automatique, et
// leur affichage (coins autour de la cible, groupes actifs, missiles). Le moteur fournit les armes (Weapons::Update) et
// fait voler les projectiles ; le jeu décide quand et sur quoi tirer.
namespace Game {
    void RegisterPlayerWeapons(Flux::Engine& engine);
    void UnregisterPlayerWeapons(Flux::Engine& engine);
}
