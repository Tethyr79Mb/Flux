#pragma once

namespace Flux {
    class EditorExtensions;
}

namespace Game {
    // Ajoute au panneau Placer de l'éditeur les objets du jeu (chasseur, cible, stations).
    void RegisterPlaceItems(Flux::EditorExtensions& extensions);
    // Aimant de l'éditeur : une arme lâchée sur un emport, ou près de lui, s'y fixe s'il l'accepte.
    void RegisterWeaponMount(Flux::EditorExtensions& extensions);
}
