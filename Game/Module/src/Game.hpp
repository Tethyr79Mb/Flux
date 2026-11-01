#pragma once

namespace Flux {
    class Engine;
}

namespace Game {
    // Tout le jeu : composants, systèmes, actions d'entrée, objets de l'éditeur. Appelé par les points d'entrée de la
    // DLL (Game/Module/Dll) ; Unregister retire ce que Register a ajouté.
    void Register(Flux::Engine& engine);
    void Unregister(Flux::Engine& engine);
}
