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

        // Pilotage sur six axes, façon combat spatial avec assistance de vol. Le vaisseau garde sa vitesse dans le
        // monde (inertie) ; ses propulseurs, limités à ShipComponent::acceleration sur chaque axe, la ramènent vers
        // celle commandée : vers l'avant la vitesse de croisière (Z/S la changent, elle reste sans commande), sur les
        // côtés et en hauteur le glissement tant que les touches sont tenues, sinon zéro (il reste à sa hauteur). En
        // virage, il dérive donc avant de repartir vers son nez. Le frein vise l'arrêt complet. Lacet et tangage à la
        // souris, roulis aux touches, atteints au plus vite de ShipComponent::turnAcceleration. Posé au sol, il ne
        // fait que décoller : ni rotation ni déplacement à plat.
        void Pilot(ShipComponent& ship, double deltaSeconds, const SeatInput& input) {
            const float delta = static_cast<float>(deltaSeconds);
            if (delta <= 0.0f) {
                return; // pause
            }
            const auto approach = [](float& value, float target, float step) { value += std::clamp(target - value, -step, step); };

            // Rotation voulue : le mouvement de la souris pendant cette image et le roulis, bornés à la vitesse de
            // rotation ; la vitesse de rotation y va progressivement.
            const float rate = ship.turnRate;
            constexpr float Degrees = 180.0f / std::numbers::pi_v<float>; // vitesses de rotation en °/s, souris en radians
            Math::Vector3f turn{std::clamp(input.mousePitch * Degrees / delta, -rate, rate),
                                std::clamp(input.mouseYaw * Degrees / delta, -rate, rate), // vers la gauche
                                -input.roll * rate};                                       // +z : vers la gauche
            if (ship.grounded) {
                turn = {};
                ship.angularVelocity = {};
            }
            const float turnStep = ship.turnAcceleration * delta;
            approach(ship.angularVelocity.x, turn.x, turnStep);
            approach(ship.angularVelocity.y, turn.y, turnStep);
            approach(ship.angularVelocity.z, turn.z, turnStep);
            // Inertie : la physique tourne le vaisseau de cette rotation pendant l'image ; sa vitesse, dans ses axes,
            // tourne d'autant dans l'autre sens pour rester la même dans le monde.
            if (const float angle = Math::Length(ship.angularVelocity) * delta / Degrees; angle > 0.0f) {
                ship.velocity = Math::Quaternionf::FromAxisAngle(ship.angularVelocity, angle).Conjugate().Rotate(ship.velocity);
            }

            // Vitesse voulue, dans les axes du vaisseau.
            if (input.brake || ship.grounded) {
                ship.cruiseSpeed = 0.0f;
            } else {
                ship.cruiseSpeed = std::clamp(ship.cruiseSpeed + input.thrust * ship.acceleration * delta, -ship.maxSpeed * 0.25f, ship.maxSpeed);
            }
            Math::Vector3f wanted{input.strafe * ship.strafeSpeed, input.lift * ship.strafeSpeed, -ship.cruiseSpeed};
            if (input.brake) {
                wanted = {};
            } else if (ship.grounded) {
                wanted = {0.0f, wanted.y, 0.0f}; // seulement décoller (ou appuyer sur le sol)
            }
            const float step = ship.acceleration * delta;
            approach(ship.velocity.x, wanted.x, step);
            approach(ship.velocity.y, wanted.y, step);
            approach(ship.velocity.z, wanted.z, step);
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
            // Aux commandes : le vaisseau garde la vitesse qu'il avait vers l'avant.
            if (state == SeatState::Seated && scene.Get<SeatComponent>(character->seat).pilot) {
                if (const Entity ship = scene.GetShip(character->seat); ship != NullEntity) {
                    ShipComponent& component = scene.Get<ShipComponent>(ship);
                    component.cruiseSpeed = -component.velocity.z;
                }
            }
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
