#pragma once

namespace Flux {
    class Engine;
}

namespace Game {
    // Jeu en réseau. Le bouton « Rejoindre » d'une interface (fonction C++) se connecte au serveur (champ lié
    // « Adresse du serveur », --serveur adresse[:port] ou 127.0.0.1) sous le nom du champ « Nom du joueur » (--joueur
    // l'emporte ; les deux gardés dans Joueur.json à côté de l'exe). Le jeu ne simule plus son monde : il charge la
    // scène que le serveur lui dit, y applique les instantanés reçus (interpolés, 100 ms en retard) et lui envoie les
    // commandes du joueur. Coupé, le monde reste figé derrière la fenêtre « Connexion perdue » (texte lié « Raison de la
    // coupure ») ; « Quitter le serveur » ramène à la scène d'où l'on a rejoint.
    void RegisterServerConnection(Flux::Engine& engine);
    void UnregisterServerConnection(Flux::Engine& engine);
}
