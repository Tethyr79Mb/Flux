#pragma once

#include "Flux/Export.hpp"
#include "Flux/Scene/Components.hpp"
#include "Flux/Scene/Weapons.hpp"

#include <vector>

namespace Flux {
    class Scene;

    // IA de combat des vaisseaux ennemis (CombatAIComponent) : une machine à états (patrouille, attaque, rupture,
    // fuite) qui mène le vaisseau comme le pilote automatique (vitesse et rotation de ShipComponent) et tire avec ses
    // armes (Weapons::Update). Seulement les vaisseaux à la racine de la scène, hors warp et pilote automatique.
    namespace CombatAI {
        // Choix de l'état et du cap. target : le vaisseau du joueur (NullEntity : aucun, ils patrouillent). Pendant le
        // jeu, avant la physique (comme Ships::Update).
        FLUX_API void Steer(Scene& scene, double deltaSeconds, Entity target);

        // Tir de ceux qui ont la cible devant eux ; après la physique, comme les armes du joueur.
        FLUX_API void Fire(Scene& scene, double deltaSeconds, Entity target, std::vector<Projectile>& projectiles);
    }
}
