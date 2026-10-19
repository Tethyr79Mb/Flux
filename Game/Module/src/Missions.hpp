#pragma once

namespace Flux {
    class Engine;
}

// Missions du jeu : parler à un donneur (F), accepter, suivre le groupe d'ennemis jusqu'à sa destruction. Le moteur
// fournit les briques (MissionGiverComponent, Missions::SpawnEnemies...) ; le déroulement est ici.
namespace Game {
    void RegisterMissions(Flux::Engine& engine);
    void UnregisterMissions(Flux::Engine& engine);
}
