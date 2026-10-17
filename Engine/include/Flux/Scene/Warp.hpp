#pragma once

#include "Flux/Export.hpp"
#include "Flux/Math/WorldPosition.hpp"
#include "Flux/Scene/Components.hpp"

#include <span>
#include <vector>

namespace Flux {
    class Scene;

    // Warp : pour les grandes distances (le mode voyage, Tab, n'est qu'un déplacement accéléré). Le pilote vise une
    // destination (station, planète, objectif de mission) ; le vaisseau se tourne vers elle en chargeant, puis y file
    // de plus en plus vite, sans collision, et en sort de lui-même à son arrivée. Il ralentit près des planètes et
    // s'arrête à la limite de leur atmosphère : il ne les traverse jamais.
    namespace Warp {
        inline constexpr double MinDistance = 20000.0;  // m : plus près, le mode voyage suffit
        inline constexpr double StationExit = 4000.0;   // m d'une station ou d'un objectif : de quoi s'amarrer (Docking::Range)
        inline constexpr float AimAngle = 30.0f;        // ° du nez : destination visée
        inline constexpr float ChargeTime = 3.0f;       // s de charge avant de partir, nez vers la destination

        struct Destination
        {
            Entity entity = NullEntity; // station ou planète ; NullEntity : un lieu (objectif de mission)
            Math::WorldPosition position;
            double exit = StationExit; // m autour de la position : là où il sort du warp
        };

        // Stations (vaisseaux qui ont des quais) et planètes de la scène, sauf le vaisseau lui-même, ajoutées à
        // destinations (vidée d'abord : appelée à chaque image, elle garde sa mémoire).
        FLUX_API void FindDestinations(const Scene& scene, Entity ship, std::vector<Destination>& destinations);

        // Destination la plus proche du nez (à moins de AimAngle), assez loin pour le warp : son indice, -1 sinon.
        FLUX_API int Aim(const Scene& scene, Entity ship, std::span<const Destination> destinations);

        enum class EngageResult : std::uint8_t {
            Started,
            Unavailable, // pas de moteur de warp, amarré, posé, sous pilote automatique, dans une atmosphère
            TooClose,
        };
        FLUX_API EngageResult Engage(Scene& scene, Entity ship, const Destination& destination);

        // Coupe le warp : le vaisseau garde sa vitesse normale (ShipComponent::maxSpeed au plus).
        FLUX_API void Stop(Scene& scene, Entity ship);

        // Charge et vol des vaisseaux en warp ; pendant le jeu, après Ships::Update (il prend la main) et avant la
        // physique, qui les déplace sans collision.
        FLUX_API void Update(Scene& scene, double deltaSeconds);

        // Distance restante jusqu'à la sortie (m), 0 hors warp.
        FLUX_API double GetRemaining(const Scene& scene, Entity ship);
    }
}
