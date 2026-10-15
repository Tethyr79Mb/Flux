#include "Flux/Scene/Ships.hpp"

#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/Seats.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Flux::Ships {
    namespace {

        // Pilotage sur six axes, façon combat spatial avec assistance de vol. Le vaisseau garde sa vitesse dans le
        // monde (inertie) ; ses propulseurs, limités à ShipComponent::acceleration sur chaque axe, la ramènent vers
        // celle commandée : vers l'avant la vitesse de croisière (Z/S la changent, elle reste sans commande), sur les
        // côtés et en hauteur le glissement tant que les touches sont tenues, sinon zéro (il reste à sa hauteur). En
        // virage, il dérive donc avant de repartir vers son nez. Le frein vise l'arrêt complet. Lacet et tangage à la
        // souris, roulis aux touches, atteints au plus vite de ShipComponent::turnAcceleration. Posé au sol, il ne
        // fait que décoller : ni rotation ni déplacement à plat. Avec ShipComponent::levelAssist et une gravité
        // (ShipInput::up), le lacet tourne autour de la verticale du lieu (virer garde l'horizon à plat) et, sans
        // roulis commandé, les ailes reviennent à plat ; le tangage reste où le pilote le laisse (piquer pour se poser).
        constexpr float RetroFactor = 3.0f;          // poussée pour ralentir, en multiple de ShipComponent::acceleration
        constexpr float SpaceSlowdownTime = 5.0f;    // fonte de la vitesse du voyage après son arrêt (constante de temps, s)
        constexpr float AirSlowdownTime = 1.5f;
        constexpr float ThrottleRate = 2.5f;         // poussée des moteurs (flammes) : de coupés à fond en 0,4 s

        void Pilot(ShipComponent& ship, const Math::Quaternionf& rotation, double deltaSeconds, const ShipInput& input) {
            const float delta = static_cast<float>(deltaSeconds);
            if (delta <= 0.0f) {
                return; // pause
            }
            const auto approach = [](float& value, float target, float step) { value += std::clamp(target - value, -step, step); };

            // Mode voyage : allumé ou éteint par Tab, coupé par le frein ou au sol ; son régime y va en
            // ShipComponent::travelSpoolTime secondes.
            if (input.travel) {
                ship.travel = !ship.travel;
            }
            // Seulement dans l'espace : il ne s'allume pas dans une atmosphère et s'y coupe en y entrant.
            if (input.brake || ship.grounded || input.air > 0.0f) {
                ship.travel = false;
            }
            const float spool = ship.travelSpoolTime > 0.0f ? delta / ship.travelSpoolTime : 1.0f;
            approach(ship.travelLevel, ship.travel ? 1.0f : 0.0f, spool);
            const float travel = 1.0f + (std::max(ship.travelFactor, 1.0f) - 1.0f) * ship.travelLevel;

            // Rotation voulue : le mouvement de la souris pendant cette image et le roulis, bornés à la vitesse de
            // rotation (le tiers en mode voyage) ; la vitesse de rotation y va progressivement.
            const float rate = ship.turnRate * (1.0f - 0.67f * ship.travelLevel);
            constexpr float Degrees = 180.0f / std::numbers::pi_v<float>; // vitesses de rotation en °/s, souris en radians
            Math::Vector3f turn{std::clamp(input.mousePitch * Degrees / delta, -rate, rate),
                                std::clamp(input.mouseYaw * Degrees / delta, -rate, rate), // vers la gauche
                                -input.roll * rate};                                       // +z : vers la gauche
            if (ship.levelAssist && Math::Length(input.up) > 0.5f) {
                // Haut du lieu dans les axes du vaisseau.
                const Math::Vector3f up = rotation.Conjugate().Rotate(input.up);
                turn = Math::Vector3f{turn.x, 0.0f, turn.z} + up * turn.y;
                if (input.roll == 0.0f) {
                    // Inclinaison : angle de l'aile droite au-dessus de l'horizon (sur le dos : au-delà de 90°).
                    // Indéfinie nez à la verticale : l'aide s'efface.
                    const float horizontal = std::sqrt(up.x * up.x + up.y * up.y);
                    const float weight = std::clamp((horizontal - 0.1f) / 0.3f, 0.0f, 1.0f);
                    const float bank = std::atan2(up.x, up.y);
                    turn.z -= bank * Degrees * ship.levelRate * weight * weight * (3.0f - 2.0f * weight);
                }
                turn.x = std::clamp(turn.x, -rate, rate);
                turn.y = std::clamp(turn.y, -rate, rate);
                turn.z = std::clamp(turn.z, -rate, rate);
            }
            // Courbure de la planète : à plat, le vaisseau tourne avec l'horizon qui défile sous lui (vitesse horizontale /
            // distance au centre). Sinon son nez, fixe dans l'espace, pique peu à peu vers le sol, d'autant plus vite
            // qu'il va vite (5° en 10 s à 5 km/s sur 600 km de rayon), et les propulseurs ne rattrapent pas la vitesse.
            // Pas en mode voyage : là, on vise librement à travers l'espace.
            Math::Vector3f carry{};
            if (ship.levelAssist && Math::Length(input.up) > 0.5f && Math::Length(input.planetUp) > 0.5f && input.planetCenter > 0.0f
                && !ship.grounded) {
                const Math::Vector3f up = rotation.Conjugate().Rotate(input.planetUp);
                carry = Math::Cross(up, ship.velocity) * (Degrees * (1.0f - ship.travelLevel) / input.planetCenter);
                turn = turn + carry;
            }
            if (ship.grounded) {
                turn = {};
                ship.angularVelocity = {};
            }
            const float turnStep = ship.turnAcceleration * delta;
            approach(ship.angularVelocity.x, turn.x, turnStep);
            approach(ship.angularVelocity.y, turn.y, turnStep);
            approach(ship.angularVelocity.z, turn.z, turnStep);
            // Inertie : la physique tourne le vaisseau de cette rotation pendant l'image ; sa vitesse, dans ses axes,
            // tourne d'autant dans l'autre sens pour rester la même dans le monde. Sauf la part due à la courbure : la
            // vitesse tourne avec le vaisseau et reste parallèle au sol.
            const Math::Vector3f inertial = ship.angularVelocity - carry;
            if (const float angle = Math::Length(inertial) * delta / Degrees; angle > 0.0f) {
                ship.velocity = Math::Quaternionf::FromAxisAngle(inertial, angle).Conjugate().Rotate(ship.velocity);
            }

            // Dans l'air, la portance range la vitesse derrière le nez : virer ou cabrer change vraiment de direction.
            // Pas de traînée qui borne la vitesse : on fonce au ras du sol aussi vite que dans l'espace.
            const float air = std::max(input.air, 0.0f);
            // Boost : plus vite, plus fort, droit devant (sans effet en mode voyage, déjà plus rapide).
            const bool boosting = input.boost && !input.brake && !ship.grounded && ship.travelLevel == 0.0f;
            const float boost = boosting ? std::max(ship.boost, 1.0f) : 1.0f;
            const float acceleration = ship.acceleration * boost * travel;
            const float maxSpeed = ship.maxSpeed * travel;
            // En mode voyage, la vitesse suit le nez (comme dans l'air) : virer change vraiment de direction.
            float grip = std::min(1.0f, 2.0f * ship.travelLevel * delta);
            if (air > 0.0f) {
                grip = std::max(grip, std::min(1.0f, ship.airGrip * std::sqrt(air) * delta));
            }

            // Vitesse voulue, dans les axes du vaisseau. Boost et mode voyage foncent droit devant.
            if (input.brake || ship.grounded) {
                ship.cruiseSpeed = 0.0f;
            } else if (boosting) {
                ship.cruiseSpeed = std::min(std::max(ship.cruiseSpeed, 0.0f) + acceleration * delta, maxSpeed * boost);
            } else if (ship.travel) {
                ship.cruiseSpeed = std::min(std::max(ship.cruiseSpeed, 0.0f) + acceleration * delta, maxSpeed);
            } else {
                // S vers l'arrière ou Z en marche arrière : la vitesse de croisière baisse au rythme des rétrofusées.
                const bool retro = input.thrust * ship.cruiseSpeed < 0.0f;
                ship.cruiseSpeed = std::clamp(ship.cruiseSpeed + input.thrust * acceleration * (retro ? RetroFactor : 1.0f) * delta,
                                              -maxSpeed * 0.25f, maxSpeed);
            }
            Math::Vector3f wanted{input.strafe * ship.strafeSpeed, input.lift * ship.strafeSpeed, -ship.cruiseSpeed};
            if (input.brake) {
                wanted = {};
            } else if (ship.grounded) {
                wanted = {0.0f, wanted.y, 0.0f}; // seulement décoller (ou appuyer sur le sol)
            }
            // Propulseurs, plus l'air qui rattrape une part de l'écart à chaque image.
            const float step = acceleration * delta;
            approach(ship.velocity.x, wanted.x, step + std::abs(wanted.x - ship.velocity.x) * grip);
            approach(ship.velocity.y, wanted.y, step + std::abs(wanted.y - ship.velocity.y) * grip);
            // Ralentir (S, frein) pousse plus fort qu'accélérer : les rétrofusées arrêtent le vaisseau en un tiers du
            // temps qu'il a mis à prendre sa vitesse.
            const bool slowing = std::abs(wanted.z) < std::abs(ship.velocity.z) && wanted.z * ship.velocity.z >= 0.0f;
            // Moteurs principaux (flammes des réacteurs) : à fond en boost et en mode voyage, forts pour prendre de la
            // vitesse vers l'avant, et en croisière d'autant plus que la vitesse est grande (on s'attend à voir
            // brûler un vaisseau qui file, même sans frottement à vaincre dans l'espace). Coupés pour ralentir, en
            // marche arrière et posé.
            float throttle = 0.0f;
            if (boosting || ship.travelLevel > 0.0f) {
                throttle = 1.0f;
            } else if (!ship.grounded && !input.brake && !slowing && ship.cruiseSpeed > 0.0f) {
                throttle = 0.5f * std::clamp(-ship.velocity.z / std::max(maxSpeed, 0.001f), 0.0f, 1.0f);
                if (wanted.z < ship.velocity.z - 0.5f) {
                    throttle += 0.4f;
                }
            }
            approach(ship.throttle, throttle, ThrottleRate * delta);
            approach(ship.velocity.z, wanted.z, step * (slowing ? RetroFactor : 1.0f) + std::abs(wanted.z - ship.velocity.z) * grip);

            // Le mode voyage porte la vitesse au-delà de ce que tiennent les propulseurs : quand son régime retombe
            // (Tab, frein, entrée dans une atmosphère), l'excès fond de lui-même, en douceur dans l'espace (moitié en
            // 3,5 s), vite dans l'air (moitié en 1 s). Sinon les 1 000 m/s du voyage ne fondaient qu'à
            // ShipComponent::acceleration (une minute et demie) : entré trop vite dans l'atmosphère, le vaisseau filait
            // vers le sol sans répondre.
            const float normalSpeed = Math::Length(Math::Vector3f{ship.strafeSpeed, ship.strafeSpeed, ship.maxSpeed}) * std::max(ship.boost, 1.0f);
            if (const float speed = Math::Length(ship.velocity); speed > normalSpeed * travel) {
                const float slowdown = air > 0.0f ? AirSlowdownTime : SpaceSlowdownTime;
                const float limit = normalSpeed * travel + (speed - normalSpeed * travel) * std::exp(-delta / slowdown);
                ship.velocity = ship.velocity * (limit / speed);
            }
            // Vers une planète, il ne s'approche de sa limite pas plus vite que sa distance en ApproachTime secondes :
            // il ralentit à mesure qu'il arrive et y entre à vitesse normale. Le long de la planète, rien ne change.
            if (Math::Length(input.planetUp) > 0.5f) {
                constexpr float ApproachTime = 3.0f;
                const Math::Vector3f up = rotation.Conjugate().Rotate(input.planetUp);
                const float closing = -Math::Dot(ship.velocity, up);
                const float allowed = std::max(normalSpeed, std::max(input.planetDistance, 0.0f) / ApproachTime);
                if (closing > allowed) {
                    ship.velocity = ship.velocity + up * (closing - allowed);
                }
            }
        }
    }

    void Update(Scene& scene, Entity piloted, double deltaSeconds, const ShipInput& input) {
        if (piloted != NullEntity && scene.IsValid(piloted)) {
            if (auto* ship = scene.TryGet<ShipComponent>(piloted)) {
                Pilot(*ship, scene.GetWorldRotation(piloted), deltaSeconds, input);
            }
        }
        // Moteurs des vaisseaux que personne ne pilote : ils se coupent (flammes au ralenti).
        const float step = ThrottleRate * static_cast<float>(std::max(deltaSeconds, 0.0));
        for (const auto [entity, ship] : scene.GetRegistry().view<ShipComponent>().each()) {
            if (entity != piloted) {
                ship.throttle = std::max(ship.throttle - step, 0.0f);
            }
        }
    }

    Entity GetPilotedShip(const Scene& scene, Entity player) {
        const Entity seat = Seats::GetPilotSeat(scene, player);
        return seat != NullEntity ? scene.GetShip(seat) : NullEntity;
    }
}
