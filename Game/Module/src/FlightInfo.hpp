#pragma once

#include <string>
#include <string_view>

namespace Flux {
    class Engine;
}

// Infos de vol du vaisseau piloté, en bas de l'écran (sorties du moteur le 2026-11-01) : vitesse mesurée, mode voyage,
// vitesse verticale, altitude et air près d'une planète (Engine::GetFlightState), un message bref et les lignes des
// autres systèmes du jeu (navigation, armes).
namespace Game::FlightInfo {
    // Lignes ajoutées sous les infos de vol, une source chacune (nom choisi par le système, dans l'ordre du premier
    // appel) ; vide : aucune. Gardées jusqu'au prochain appel, effacées avec la scène.
    void SetExtra(std::string_view source, std::string_view text);
    // Message bref sous les infos de vol (« Warp coupé »...), effacé après quelques secondes de jeu.
    void ShowMessage(std::string message);
}

namespace Game {
    // Avant les autres affichages du jeu : les infos de vol sont dessinées dessous.
    void RegisterFlightInfo(Flux::Engine& engine);
    void UnregisterFlightInfo(Flux::Engine& engine);
}
