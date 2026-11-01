#pragma once

#include <Flux/Math/WorldPosition.hpp>
#include <Flux/Scene/Components.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Flux {
    class ComponentRegistry;
    class Scene;
}

// Warp des vaisseaux (sorti du moteur le 2026-11-01).
namespace Game {
    // Warp d'un vaisseau (Warp) : il se tourne vers sa destination en chargeant, puis y file sans collision et en sort
    // de lui-même à son arrivée.
    enum class WarpStage : std::uint8_t { Off, Charging, Cruising };

    // Moteur de warp d'un vaisseau (J, vers la destination visée) : pour les grandes distances, jusqu'à speed km/s.
    // Sans lui (une station), pas de warp. Les anciennes scènes le reçoivent de « ship.warpSpeed » à la lecture.
    struct WarpDriveComponent
    {
        float speed = 1.0e7f; // km/s
        // Pendant le jeu (non enregistrés) : étape, destination, rayon autour d'elle où il sort du warp (le haut de
        // l'atmosphère d'une planète, quelques km d'une station) et temps déjà chargé (s).
        WarpStage stage = WarpStage::Off;
        Flux::Math::WorldPosition target;
        double exit = 0.0;
        float charge = 0.0f;
    };

    // Enregistre WarpDriveComponent (« warpDrive ») et la conversion de l'ancien champ « ship.warpSpeed ».
    void RegisterWarpDrive(Flux::ComponentRegistry& registry);

    // Drapeaux du moteur d'après le warp et le pilote automatique du jeu : ShipComponent::autopilot (mené par le jeu),
    // ShipComponent::passThrough (warp lancé, sans collision). Appelé par Warp et Docking à chaque changement.
    void SyncShipControl(Flux::Scene& scene, Flux::Entity ship);


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
            Flux::Entity entity = Flux::NullEntity; // station ou planète ; NullEntity : un lieu (objectif de mission)
            Flux::Math::WorldPosition position;
            double exit = StationExit; // m autour de la position : là où il sort du warp
        };

        // Stations (vaisseaux qui ont des quais) et planètes de la scène, sauf le vaisseau lui-même, ajoutées à
        // destinations (vidée d'abord : appelée à chaque image, elle garde sa mémoire).
        void FindDestinations(const Flux::Scene& scene, Flux::Entity ship, std::vector<Destination>& destinations);

        // Destination la plus proche du nez (à moins de AimAngle), assez loin pour le warp : son indice, -1 sinon.
        int Aim(const Flux::Scene& scene, Flux::Entity ship, std::span<const Destination> destinations);

        enum class EngageResult : std::uint8_t {
            Started,
            Unavailable, // pas de moteur de warp, amarré, posé, sous pilote automatique, dans une atmosphère
            TooClose,
        };
        EngageResult Engage(Flux::Scene& scene, Flux::Entity ship, const Destination& destination);

        // Coupe le warp : le vaisseau garde sa vitesse normale (ShipComponent::maxSpeed au plus).
        void Stop(Flux::Scene& scene, Flux::Entity ship);

        // Charge et vol des vaisseaux en warp ; pendant le jeu, après Ships::Update (il prend la main) et avant la
        // physique, qui les déplace sans collision.
        void Update(Flux::Scene& scene, double deltaSeconds);

        // Distance restante jusqu'à la sortie (m), 0 hors warp.
        double GetRemaining(const Flux::Scene& scene, Flux::Entity ship);
    }
}
