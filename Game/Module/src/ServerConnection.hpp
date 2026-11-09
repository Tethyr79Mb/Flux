#pragma once

namespace Flux {
    class Engine;
}

namespace Game {
    // Quand Ars Solaris est lancé avec --serveur adresse[:port] : le jeu ne simule plus son monde, il affiche celui du
    // serveur. Il charge la scène que le serveur lui dit, puis y applique les instantanés reçus (interpolés, 100 ms en
    // retard) et les entités créées pendant le jeu. Le joueur ne fait que regarder (ses commandes : étape 6b). Sans
    // --serveur (éditeur compris), rien n'est fait.
    void RegisterServerConnection(Flux::Engine& engine);
    void UnregisterServerConnection(Flux::Engine& engine);
}
