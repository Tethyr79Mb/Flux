#include "Flux/Scene/Seats.hpp"

#include "Flux/Scene/Scene.hpp"

#include <algorithm>
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

        // Siège libre le plus proche, à portée, dans le même vaisseau (le même monde physique) que le personnage.
        Entity FindSeat(const Scene& scene, Entity character) {
            const Math::WorldPosition position = scene.GetWorldPosition(character);
            const Entity ship = scene.GetShip(character);
            Entity best = NullEntity;
            double bestDistance = std::numeric_limits<double>::max();
            for (const auto [seat, component] : scene.GetRegistry().view<const SeatComponent>().each()) {
                if (scene.GetShip(seat) != ship || IsOccupied(scene, seat)) {
                    continue;
                }
                const double distance = Math::Length(Math::Delta(position, scene.GetWorldPosition(seat)));
                if (distance <= Reach && distance < bestDistance) {
                    best = seat;
                    bestDistance = distance;
                }
            }
            return best;
        }

        // Le pilote change la vitesse du vaisseau : poussée le long de son -z (sans commande, il garde sa vitesse,
        // comme dans l'espace), lacet et tangage tant que les touches sont tenues.
        void Pilot(ShipComponent& ship, double deltaSeconds, const SeatInput& input) {
            ship.velocity.z -= input.thrust * ship.acceleration * static_cast<float>(deltaSeconds);
            ship.velocity.z = std::clamp(ship.velocity.z, -ship.maxSpeed, ship.maxSpeed * 0.25f);
            ship.angularVelocity.y = input.yaw * ship.turnRate; // y positif : vers la gauche
            ship.angularVelocity.x = input.pitch * ship.turnRate;
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
