#include "Flux/Scene/Seats.hpp"

#include "Flux/Scene/Scene.hpp"

#include <algorithm>
#include <numbers>
#include <limits>

namespace Flux::Seats {
    namespace {
        bool IsOccupied(const Scene& scene, Entity seat) {
            for (const auto [entity, character] : scene.GetRegistry().view<const CharacterComponent>().each()) {
                if (character.seat == seat) {
                    return true;
                }
            }
            return false;
        }

        // Siège libre le plus proche, à sa portée (SeatComponent::reach). Il peut être dans un autre vaisseau que le
        // personnage (il y monte), pas hors de tout vaisseau quand le personnage est à bord d'un.
        Entity FindSeat(const Scene& scene, Entity character) {
            const Math::WorldPosition position = scene.GetWorldPosition(character);
            const Entity ship = scene.GetShip(character);
            Entity best = NullEntity;
            double bestDistance = std::numeric_limits<double>::max();
            for (const auto [seat, component] : scene.GetRegistry().view<const SeatComponent>().each()) {
                const Entity seatShip = scene.GetShip(seat);
                if ((seatShip != ship && seatShip == NullEntity) || IsOccupied(scene, seat)) {
                    continue;
                }
                const double distance = Math::Length(Math::Delta(position, scene.GetWorldPosition(seat)));
                if (distance <= component.reach && distance < bestDistance) {
                    best = seat;
                    bestDistance = distance;
                }
            }
            return best;
        }

        // Le personnage monte à bord du vaisseau du siège : il en devient l'enfant direct (la physique le fait
        // changer de monde), posé sur le siège. Il le reste en se relevant ; une sortie hors de la coque le fait
        // redescendre dans le monde du dehors (PhysicsWorld, comme en marchant hors d'un vaisseau).
        void Board(Scene& scene, Entity character, Entity seat) {
            const Entity ship = scene.GetShip(seat);
            if (ship == NullEntity || scene.GetShip(character) == ship) {
                return;
            }
            const Math::Vector3d delta = Math::Delta(scene.GetWorldPosition(ship), scene.GetWorldPosition(seat));
            const Math::Quaternionf inverse = scene.GetWorldRotation(ship).Conjugate();
            scene.SetParent(character, ship);
            TransformComponent& transform = scene.Get<TransformComponent>(character);
            const Math::Vector3f local = inverse.Rotate(Math::Vector3f{static_cast<float>(delta.x), static_cast<float>(delta.y),
                                                                       static_cast<float>(delta.z)});
            transform.position = {local.x, local.y, local.z};
            transform.rotation = Math::Normalize(inverse * scene.GetWorldRotation(seat));
        }

        // Pilotage sur six axes, façon combat spatial. Poussée le long de -z : sans commande, le vaisseau garde sa
        // vitesse (comme dans l'espace) ; glissement sur les côtés et montée/descente tant que les touches sont tenues
        // (lâchées, il revient à l'arrêt sur ces axes : il reste à sa hauteur) ; le frein ramène tout à l'arrêt. Les
        // vitesses changent au plus de ShipComponent::acceleration. Lacet et tangage à la souris, roulis aux touches.
        void Pilot(ShipComponent& ship, double deltaSeconds, const SeatInput& input) {
            const float delta = static_cast<float>(deltaSeconds);
            if (delta <= 0.0f) {
                return; // pause
            }
            const float step = ship.acceleration * delta;
            const auto approach = [step](float& value, float target) { value += std::clamp(target - value, -step, step); };
            if (input.brake) {
                approach(ship.velocity.x, 0.0f);
                approach(ship.velocity.y, 0.0f);
                approach(ship.velocity.z, 0.0f);
            } else {
                ship.velocity.z = std::clamp(ship.velocity.z - input.thrust * step, -ship.maxSpeed, ship.maxSpeed * 0.25f);
                approach(ship.velocity.x, input.strafe * ship.strafeSpeed);
                approach(ship.velocity.y, input.lift * ship.strafeSpeed);
            }
            // Vitesse de rotation voulue : le mouvement de la souris pendant cette image, bornée.
            const float rate = ship.turnRate;
            constexpr float Degrees = 180.0f / std::numbers::pi_v<float>; // vitesses de rotation en °/s, souris en radians
            ship.angularVelocity.y = std::clamp(input.mouseYaw * Degrees / delta, -rate, rate); // vers la gauche
            ship.angularVelocity.x = std::clamp(input.mousePitch * Degrees / delta, -rate, rate);
            ship.angularVelocity.z = -input.roll * rate; // +z : aile droite vers le haut, donc vers la gauche
        }
    }

    void Update(Scene& scene, Entity player, double deltaSeconds, const SeatInput& input, const DurationsFunction& durations) {
        if (player == NullEntity || !scene.IsValid(player)) {
            return;
        }
        auto* character = scene.TryGet<CharacterComponent>(player);
        if (!character) {
            return;
        }
        // Siège détruit ou devenu autre chose : debout sur place.
        if (character->seat != NullEntity && (!scene.IsValid(character->seat) || !scene.TryGet<SeatComponent>(character->seat))) {
            character->seat = NullEntity;
            character->seatState = SeatState::Standing;
        }
        character->seatTime += static_cast<float>(deltaSeconds);
        const auto enter = [&](SeatState state) {
            character->seatState = state;
            character->seatTime = 0.0f;
        };

        switch (character->seatState) {
            case SeatState::Standing:
                if (input.interact) {
                    if (const Entity seat = FindSeat(scene, player); seat != NullEntity) {
                        Board(scene, player, seat);
                        character->seat = seat;
                        enter(durations && durations(player).sitDown > 0.0f ? SeatState::SittingDown : SeatState::Seated);
                    }
                }
                break;
            case SeatState::SittingDown:
                if (character->seatTime >= (durations ? durations(player).sitDown : 0.0f)) {
                    enter(SeatState::Seated);
                }
                break;
            case SeatState::Seated:
                if (input.interact) {
                    if (durations && durations(player).standUp > 0.0f) {
                        enter(SeatState::StandingUp);
                    } else {
                        character->seat = NullEntity;
                        enter(SeatState::Standing);
                    }
                } else if (scene.Get<SeatComponent>(character->seat).pilot) {
                    if (const Entity ship = scene.GetShip(character->seat); ship != NullEntity) {
                        Pilot(scene.Get<ShipComponent>(ship), deltaSeconds, input);
                    }
                }
                break;
            case SeatState::StandingUp:
                if (character->seatTime >= (durations ? durations(player).standUp : 0.0f)) {
                    character->seat = NullEntity;
                    enter(SeatState::Standing);
                }
                break;
        }
    }

    Entity GetPilotSeat(const Scene& scene, Entity player) {
        if (player == NullEntity || !scene.IsValid(player)) {
            return NullEntity;
        }
        const auto* character = scene.TryGet<CharacterComponent>(player);
        if (!character || character->seatState != SeatState::Seated || !scene.IsValid(character->seat)) {
            return NullEntity;
        }
        const auto* seat = scene.TryGet<SeatComponent>(character->seat);
        return seat && seat->pilot ? character->seat : NullEntity;
    }
}
