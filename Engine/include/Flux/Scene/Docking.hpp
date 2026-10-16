#pragma once

#include "Flux/Export.hpp"
#include "Flux/Scene/Components.hpp"

#include <string>

namespace Flux {
    class Scene;

    // Pilote automatique d'amarrage : conduit un vaisseau jusqu'à un quai libre d'une station (DockComponent), l'y pose
    // et en fait un enfant de la station (on se lève et on marche dans la station), puis l'en ressort. Il pilote à la
    // place du joueur en réglant ShipComponent::velocity et angularVelocity ; la physique le déplace comme d'habitude.
    // Le vaisseau ne heurte pas la station (seulement le décor immobile du monde) : le trajet passe par l'entrée.
    namespace Docking {
        // Distance maximale (m) entre le vaisseau et un quai libre pour lancer le pilote automatique.
        inline constexpr double Range = 5000.0;

        enum class ToggleResult { Docking, Leaving, Cancelled, NoDock };

        // Touche du pilote automatique (L par défaut) : en vol, vers le quai libre le plus proche à moins de Range ;
        // amarré, repartir ; pendant le trajet, le couper (le pilote reprend la main sur place).
        FLUX_API ToggleResult Toggle(Scene& scene, Entity ship);

        // Pendant le jeu, après le pilotage (Ships::Update) et avant la physique : les vaisseaux sous pilote automatique
        // suivent leur trajet ; ceux amarrés restent immobiles, quoi que fasse le pilote.
        FLUX_API void Update(Scene& scene, double deltaSeconds);

        // Amarré : posé sur son quai, enfant de sa station.
        FLUX_API bool IsDocked(const Scene& scene, Entity ship);

        // Étape en cours pour les infos de vol (« Pilote auto : vers Quai 2 », « Amarré : Quai 2 »), vide sinon.
        FLUX_API std::string Describe(const Scene& scene, Entity ship);
    }
}
