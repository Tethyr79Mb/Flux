#include "Scene/Registration/Registration.hpp"

#include "Scene/Photometry.hpp"

#include <numbers>

// Vaisseaux : réacteurs, intérieur, pilotage, sièges.
namespace Flux {
    void RegisterShipComponents(ComponentRegistry& registry) {
        registry.Register<ThrusterComponent>("thruster", "Réacteur")
            .Field("color", "Couleur", &ThrusterComponent::color, {.alpha = false})
            .Field("width", "Largeur de la tuyère (m)", &ThrusterComponent::width,
                   {.speed = 0.01, .min = 0.01, .max = 1.0e3, .decimals = 2})
            .Field("height", "Hauteur de la tuyère (m)", &ThrusterComponent::height,
                   {.speed = 0.01, .min = 0.01, .max = 1.0e3, .decimals = 2})
            .Field("length", "Longueur à fond (m)", &ThrusterComponent::length,
                   {.speed = 0.05, .min = 0.01, .max = 1.0e4, .decimals = 2})
            .Field("intensity", "Éclat", &ThrusterComponent::intensity,
                   {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("idle", "Poussée au ralenti", &ThrusterComponent::idle,
                   {.speed = 0.01, .min = 0.0, .max = 1.0, .decimals = 2})
            .Field("lightLumens", "Lumière à fond (lumens)", &ThrusterComponent::lightLumens,
                   {.speed = 1.0e4, .min = 0.0, .max = 1.0e10, .decimals = 0})
            // Avant les unités physiques : intensité d'une lumière affaiblie comme 1 / (1 + d²), 1 valant le plein
            // soleil.
            .ConvertedField("light", [](ThrusterComponent& thruster, double light) {
                thruster.lightLumens = static_cast<float>(light * Photometry::LuxPerUnit * 4.0 * std::numbers::pi);
            });
        registry.Register<InteriorComponent>("interior", "Intérieur")
            .Field("size", "Taille (m)", &InteriorComponent::size, {.speed = 0.01, .min = 0.0, .max = 1.0e4})
            .Field("ambient", "Lumière ambiante (couleur)", &InteriorComponent::ambient, {.alpha = false})
            .Field("luminance", "Lumière ambiante (cd/m²)", &InteriorComponent::luminance,
                   {.speed = 0.1, .min = 0.0, .max = 1.0e5, .decimals = 1})
            // Avant les unités physiques : réglée sur le plein soleil. Pas de conversion : la valeur réelle par défaut.
            .RemovedField("intensity")
            .Field("fade", "Fondu hors des bords (m)", &InteriorComponent::fade,
                   {.speed = 0.01, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("exposure", "Exposition, caméra dedans (stops)", &InteriorComponent::exposure,
                   {.speed = 0.02, .min = -8.0, .max = 8.0, .decimals = 2})
            .Field("minEv100", "EV100 minimum dedans", &InteriorComponent::minEv100,
                   {.speed = 0.05, .min = -6.0, .max = 20.0, .decimals = 1})
            .Field("maxEv100", "EV100 maximum dedans", &InteriorComponent::maxEv100,
                   {.speed = 0.05, .min = -6.0, .max = 20.0, .decimals = 1});
        registry.Register<ShipComponent>("ship", "Vaisseau")
            .Field("gravity", "Gravité à bord (m/s²)", &ShipComponent::gravity,
                   {.speed = 0.05, .min = 0.0, .max = 100.0, .decimals = 2})
            .Field("velocity", "Vitesse au départ (m/s)", &ShipComponent::velocity,
                   {.speed = 0.1, .min = -1.0e5, .max = 1.0e5})
            .Field("angularVelocity", "Rotation au départ (°/s)", &ShipComponent::angularVelocity,
                   {.speed = 0.5, .min = -720.0, .max = 720.0})
            .Field("maxSpeed", "Vitesse max (m/s)", &ShipComponent::maxSpeed,
                   {.speed = 0.5, .min = 0.0, .max = 1.0e5, .decimals = 1})
            .Field("acceleration", "Accélération (m/s²)", &ShipComponent::acceleration,
                   {.speed = 0.1, .min = 0.0, .max = 1.0e4, .decimals = 1})
            .Field("turnRates", "Virage : tangage, lacet, roulis (°/s)", &ShipComponent::turnRate,
                   {.speed = 0.5, .min = 0.0, .max = 720.0, .decimals = 1})
            .Field("turnAccelerations", "Accélération du virage (°/s²)", &ShipComponent::turnAcceleration,
                   {.speed = 1.0, .min = 0.0, .max = 1.0e4, .decimals = 1})
            .Field("strafeSpeed", "Glissement (m/s)", &ShipComponent::strafeSpeed,
                   {.speed = 0.1, .min = 0.0, .max = 1.0e4, .decimals = 1})
            .Field("levelAssist", "Stabiliser à plat", &ShipComponent::levelAssist)
            .Field("levelRate", "Retour à plat (1/s)", &ShipComponent::levelRate,
                   {.speed = 0.05, .min = 0.0, .max = 20.0, .decimals = 2})
            .Field("airGrip", "Tenue dans l'air (1/s)", &ShipComponent::airGrip,
                   {.speed = 0.05, .min = 0.0, .max = 50.0, .decimals = 2})
            .Field("boost", "Boost (x)", &ShipComponent::boost, {.speed = 0.05, .min = 1.0, .max = 20.0, .decimals = 2})
            .Field("travelFactor", "Mode voyage (x)", &ShipComponent::travelFactor,
                   {.speed = 0.1, .min = 1.0, .max = 1000.0, .decimals = 1})
            .Field("travelSpoolTime", "Mode voyage : mise en route (s)", &ShipComponent::travelSpoolTime,
                   {.speed = 0.05, .min = 0.0, .max = 60.0, .decimals = 1})
            .Field("fallsWithoutPilot", "Tombe sans pilote", &ShipComponent::fallsWithoutPilot)
            // Avant : une seule vitesse pour les trois axes, et une accélération réglée pour suivre la souris comme une
            // caméra (360 °/s²), sans sens avec le manche. Vaisseau plus lent qu'avant (le chasseur de 90 °/s passe à
            // 70 en tangage), lacet plus lent, roulis plus rapide, et chaque axe atteint sa vitesse en 0,8 s.
            .ConvertedField("turnRate", [](ShipComponent& ship, double rate) {
                ship.turnRate = Math::Vector3f{0.78f, 0.61f, 1.44f} * static_cast<float>(rate);
                ship.turnAcceleration = ship.turnRate * 1.25f;
            })
            .RemovedField("turnAcceleration");
        registry.Register<SeatComponent>("seat", "Siège")
            .Field("pilot", "Poste de pilotage", &SeatComponent::pilot)
            .Field("eye", "Œil du pilote", &SeatComponent::eye, {.speed = 0.01})
            .Field("exit", "Sortie", &SeatComponent::exit, {.speed = 0.01})
            .Field("reach", "Portée (m)", &SeatComponent::reach, {.speed = 0.05, .min = 0.5, .max = 50.0});
    }
}
