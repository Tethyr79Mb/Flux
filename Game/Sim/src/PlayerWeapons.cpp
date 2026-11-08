#include "PlayerWeapons.hpp"

#include "GameSim.hpp"
#include "Weapons.hpp"

#include <Flux/Core/Simulation.hpp>
#include <Flux/Scene/Scene.hpp>
#include <Flux/Scene/Ships.hpp>

#include <format>
#include <iterator>

namespace Game::PlayerWeapons {
    namespace {
        using namespace Flux;

        // Les tourelles visent le point regardé, à cette distance devant la caméra (m) : leurs tirs s'y croisent.
        constexpr float AimDistance = 700.0f;

        // Les armes du vaisseau que pilote player, d'après sa commande.
        void FireFor(Simulation& simulation, const PlayerWeaponsState& weapons, Entity player, double deltaSeconds) {
            Scene& scene = simulation.GetScene();
            const auto* character = player != NullEntity ? scene.TryGet<CharacterComponent>(player) : nullptr;
            const Entity ship = player != NullEntity ? Ships::GetPilotedShip(scene, player) : NullEntity;
            if (!character || character->seat == NullEntity || ship == NullEntity) {
                if (player != NullEntity) {
                    scene.GetRegistry().remove<PilotWeaponsComponent>(player); // lâche le verrou en quittant les commandes
                }
                return;
            }
            const PlayerCommand& command = simulation.GetPlayerCommand(player);
            PilotWeaponsComponent& state = scene.GetRegistry().get_or_emplace<PilotWeaponsComponent>(player);
            WeaponInput input;
            // Tir automatique (choix de Bruno) : un appui active le groupe, un nouvel appui l'arrête. Ses canons
            // tirent seuls dès que la cible verrouillée est à leur portée ; ses lance-missiles en lancent un, puis
            // le groupe s'arrête (un appui, un missile).
            std::uint16_t pressed = 0;
            for (std::size_t group = 0; group < weapons.groupActions.size(); ++group) {
                pressed |= command.WasPressed(weapons.groupActions[group]) ? static_cast<std::uint16_t>(1u << group) : 0;
            }
            state.activeGroups ^= pressed;
            input.aim = true;
            input.aimPoint = command.viewPosition;
            input.aimPoint += Math::Vector3d(command.viewForward * AimDistance);
            // Verrouillage à l'appui sur une touche de tir (choix de Bruno) : la cible au centre de la vue. Rien
            // dans le viseur : le verrou d'avant reste. Il tient ensuite hors du viseur, tant que la cible est à
            // portée.
            if (pressed != 0) {
                if (const Entity target = Weapons::FindLockTarget(scene, ship, command.viewPosition, command.viewForward);
                    target != NullEntity) {
                    state.lockTarget = target;
                }
            }
            if (!Weapons::KeepsLock(scene, ship, state.lockTarget, command.viewPosition)) {
                state.lockTarget = NullEntity;
                state.activeGroups = 0; // cible perdue ou détruite : tout s'arrête
            }
            input.target = state.lockTarget;
            input.active = state.activeGroups;
            state.activeGroups &= static_cast<std::uint16_t>(~Weapons::Update(scene, ship, deltaSeconds, input, simulation.GetProjectiles()));
        }
    }

    void DeclareActions(PlayerWeaponsState& state, CommandActions& actions) {
        for (std::size_t group = 0; group < GroupActions.size(); ++group) {
            state.groupActions[group] = actions.Declare(GroupActions[group]);
        }
    }

    const PilotWeaponsComponent* FindState(const Scene& scene, Entity player) {
        return player != NullEntity && scene.IsValid(player) ? scene.GetRegistry().try_get<PilotWeaponsComponent>(player) : nullptr;
    }

    void Fire(Simulation& simulation, SimState& state, double deltaSeconds) {
        // Un seul joueur pour l'instant ; le serveur bouclera sur les siens.
        const Entity player = simulation.GetPlayer();
        FireFor(simulation, state.weapons, player, deltaSeconds);

        // Infos de vol du joueur : groupes actifs et missiles restants (moins de 30 caractères : pas d'allocation après
        // le premier pas).
        const Scene& scene = simulation.GetScene();
        const PilotWeaponsComponent* weapons = FindState(scene, player);
        const std::uint16_t activeGroups = weapons ? weapons->activeGroups : 0;
        const Entity ship = player != NullEntity ? Ships::GetPilotedShip(scene, player) : NullEntity;
        std::string& info = state.weapons.info;
        info.clear();
        if (activeGroups != 0) {
            info += "Tir auto ";
            for (int group = 0; group < 10; ++group) {
                if (activeGroups & (1u << group)) {
                    std::format_to(std::back_inserter(info), " {}", (group + 1) % 10); // groupe 10 : touche 0
                }
            }
        }
        if (const int missiles = ship != NullEntity ? Weapons::GetMissileCount(scene, ship) : 0; missiles != 0) {
            info += info.empty() ? "" : "\n";
            info += "Missiles";
            if (missiles > 0) {
                std::format_to(std::back_inserter(info), "  {}", missiles);
            }
        }
    }

    void Reset(Scene& scene, PlayerWeaponsState& state) {
        scene.GetRegistry().clear<PilotWeaponsComponent>();
        state.info.clear();
    }
}
