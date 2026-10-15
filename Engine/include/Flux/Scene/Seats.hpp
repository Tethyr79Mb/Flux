#pragma once

#include "Flux/Export.hpp"
#include "Flux/Scene/Components.hpp"

#include <functional>

namespace Flux {
    class Scene;

    // Commande du joueur pour les sièges (SeatComponent) ; le pilotage du vaisseau passe par ShipInput (Ships.hpp).
    struct SeatInput
    {
        bool interact = false; // F : s'asseoir sur le siège le plus proche, ou se relever
    };

    // Le joueur s'assoit et se relève : voir SeatComponent. La physique pose le personnage sur son siège
    // (CharacterComponent::seat), l'animation suit CharacterComponent::seatState.
    namespace Seats {
        // Durées des animations « s'asseoir » et « se lever » d'un personnage, en secondes (0 : immédiat).
        struct Durations
        {
            float sitDown = 0.0f;
            float standUp = 0.0f;
        };
        using DurationsFunction = std::function<Durations(Entity character)>;

        // player : personnage dirigé par le joueur (NullEntity : aucun). Pendant le jeu, après les commandes et
        // avant la physique.
        FLUX_API void Update(Scene& scene, Entity player, double deltaSeconds, const SeatInput& input,
                             const DurationsFunction& durations);

        // Poste de pilotage où le joueur est assis (étape Seated), NullEntity sinon.
        FLUX_API Entity GetPilotSeat(const Scene& scene, Entity player);
    }
}
