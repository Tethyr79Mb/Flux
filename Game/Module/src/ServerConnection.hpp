#pragma once

namespace Flux {
    class Engine;
}

namespace Game {
    // Connexion à un serveur de jeu quand Ars Solaris est lancé avec --serveur adresse[:port] : connexion, refus
    // (versions différentes) et déconnexion dans le journal. Rien d'autre ne passe avant la réplication (étape 6) ; le
    // jeu continue de simuler sa scène lui-même. Sans --serveur (éditeur compris), rien n'est fait.
    void RegisterServerConnection(Flux::Engine& engine);
    void UnregisterServerConnection(Flux::Engine& engine);
}
