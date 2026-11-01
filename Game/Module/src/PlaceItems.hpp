#pragma once

namespace Flux {
    class EditorExtensions;
}

namespace Game {
    // Ajoute au panneau Placer de l'éditeur les objets du jeu (chasseur, cible, stations).
    void RegisterPlaceItems(Flux::EditorExtensions& extensions);
}
