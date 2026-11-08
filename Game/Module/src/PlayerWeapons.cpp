#include "PlayerWeapons.hpp"

#include "FlightInfo.hpp"
#include "Weapons.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Scene/Picking.hpp>
#include <Flux/Input/InputActions.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Renderer/Renderer.hpp>
#include <Flux/Scene/Components.hpp>
#include <Flux/Scene/Ships.hpp>
#include <Flux/UI/DrawList.hpp>
#include <Flux/UI/HudText.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace Game {
    namespace {
        using namespace Flux;

        // Actions des groupes d'armes (WeaponComponent::group), sur les chiffres au-dessus des lettres (choix de
        // Bruno) : sur le Chasseur, 1 le nez, 2 les autocanons, 3 les missiles.
        constexpr std::array<std::string_view, 10> GroupActions{
            "Groupe d'armes 1", "Groupe d'armes 2", "Groupe d'armes 3", "Groupe d'armes 4", "Groupe d'armes 5",
            "Groupe d'armes 6", "Groupe d'armes 7", "Groupe d'armes 8", "Groupe d'armes 9", "Groupe d'armes 10"};
        constexpr std::array GroupKeys{Key::Digit1, Key::Digit2, Key::Digit3, Key::Digit4, Key::Digit5,
                                       Key::Digit6, Key::Digit7, Key::Digit8, Key::Digit9, Key::Digit0};
        // Les tourelles visent le point regardé, à cette distance devant la caméra (m) : leurs tirs s'y croisent.
        constexpr float AimDistance = 700.0f;

        // Armes d'un joueur, sur son personnage : chacun a son verrou et ses groupes actifs. Le temps du jeu
        // seulement (ni inspecteur ni sauvegarde) : un chargement repart sans verrou.
        struct PilotWeaponsComponent
        {
            Entity lockTarget = NullEntity; // cible verrouillée par les armes du vaisseau piloté
            std::uint16_t activeGroups = 0; // groupes en tir automatique (bit 0 : touche 1)
        };

        std::array<InputActionId, 10> g_groupActions; // bits des groupes dans PlayerCommand
        std::string g_info; // lignes ajoutées aux infos de vol, gardée d'une image à l'autre

        // Les armes du vaisseau que pilote player, d'après sa commande.
        void FireFor(Engine& engine, Entity player, double deltaSeconds) {
            Scene& scene = engine.GetScene();
            const auto* character = player != NullEntity ? scene.TryGet<CharacterComponent>(player) : nullptr;
            const Entity ship = player != NullEntity ? Ships::GetPilotedShip(scene, player) : NullEntity;
            if (!character || character->seat == NullEntity || ship == NullEntity) {
                if (player != NullEntity) {
                    scene.GetRegistry().remove<PilotWeaponsComponent>(player); // lâche le verrou en quittant les commandes
                }
                return;
            }
            const PlayerCommand& command = engine.GetPlayerCommand(player);
            PilotWeaponsComponent& state = scene.GetRegistry().get_or_emplace<PilotWeaponsComponent>(player);
            WeaponInput input;
            // Tir automatique (choix de Bruno) : un appui active le groupe, un nouvel appui l'arrête. Ses canons
            // tirent seuls dès que la cible verrouillée est à leur portée ; ses lance-missiles en lancent un, puis
            // le groupe s'arrête (un appui, un missile).
            std::uint16_t pressed = 0;
            for (std::size_t group = 0; group < g_groupActions.size(); ++group) {
                pressed |= command.WasPressed(g_groupActions[group]) ? static_cast<std::uint16_t>(1u << group) : 0;
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
            state.activeGroups &= static_cast<std::uint16_t>(~Weapons::Update(scene, ship, deltaSeconds, input, engine.GetProjectiles()));
        }

        const PilotWeaponsComponent* FindState(const Scene& scene, Entity player) {
            return player != NullEntity && scene.IsValid(player) ? scene.GetRegistry().try_get<PilotWeaponsComponent>(player) : nullptr;
        }

        void Fire(Engine& engine, double deltaSeconds) {
            // Un seul joueur pour l'instant ; le serveur bouclera sur les siens.
            const Entity player = engine.GetPlayer();
            FireFor(engine, player, deltaSeconds);

            // Infos de vol du joueur de cet écran : groupes actifs et missiles restants (moins de 30 caractères : pas
            // d'allocation après la première image).
            const Scene& scene = engine.GetScene();
            const PilotWeaponsComponent* state = FindState(scene, player);
            const std::uint16_t activeGroups = state ? state->activeGroups : 0;
            const Entity ship = player != NullEntity ? Ships::GetPilotedShip(scene, player) : NullEntity;
            g_info.clear();
            if (activeGroups != 0) {
                g_info += "Tir auto ";
                for (int group = 0; group < 10; ++group) {
                    if (activeGroups & (1u << group)) {
                        std::format_to(std::back_inserter(g_info), " {}", (group + 1) % 10); // groupe 10 : touche 0
                    }
                }
            }
            if (const int missiles = ship != NullEntity ? Weapons::GetMissileCount(scene, ship) : 0; missiles != 0) {
                g_info += g_info.empty() ? "" : "\n";
                g_info += "Missiles";
                if (missiles > 0) {
                    std::format_to(std::back_inserter(g_info), "  {}", missiles);
                }
            }
            FlightInfo::SetExtra("Armes", g_info);
        }

        // Cible verrouillée : quatre coins autour d'elle et sa distance.
        void DrawLock(Engine& engine, double) {
            const Scene& scene = engine.GetScene();
            const PilotWeaponsComponent* state = FindState(scene, engine.GetPlayer());
            const Entity lockTarget = state ? state->lockTarget : NullEntity;
            if (lockTarget == NullEntity || !scene.IsValid(lockTarget)) {
                return;
            }
            const Camera& camera = engine.GetCamera();
            const Math::Vector2i size = engine.GetWindow().GetSize();
            const Math::Vector2f screen{static_cast<float>(size.x), static_cast<float>(size.y)};
            const Math::Vector3d offset = Math::Delta(camera.position, scene.GetWorldPosition(lockTarget));
            const auto point = ScreenProjection(camera, {}, screen).ToScreen(offset);
            if (!point) {
                return;
            }
            UI::DrawList& list = engine.GetRenderer().GetOverlay();
            constexpr float Half = 22.0f;
            constexpr float Corner = 9.0f;
            constexpr Math::LinearColor LockColor{1.0f, 0.35f, 0.2f, 0.95f};
            for (const float sx : {-1.0f, 1.0f}) {
                for (const float sy : {-1.0f, 1.0f}) {
                    const Math::Vector2f tip{point->x + sx * Half, point->y + sy * Half};
                    list.AddLine(tip, {tip.x - sx * Corner, tip.y}, LockColor, 2.0f);
                    list.AddLine(tip, {tip.x, tip.y - sy * Corner}, LockColor, 2.0f);
                }
            }
            const std::string label = FormatDistance(Math::Length(offset));
            constexpr float LabelSize = 15.0f;
            const Math::Vector2f labelExtent = list.MeasureText(label, LabelSize);
            list.AddText({point->x - labelExtent.x * 0.5f, point->y + Half + 4.0f}, label, LabelSize, LockColor);
        }

        void Reset(Engine& engine, double) {
            engine.GetScene().GetRegistry().clear<PilotWeaponsComponent>();
            g_info.clear();
        }

        constexpr const char* FireSystem = "Armes du joueur : tir";
        constexpr const char* HudSystem = "Armes du joueur : affichage";
        constexpr const char* ResetSystem = "Armes du joueur : oubli";
    }

    void RegisterPlayerWeapons(Engine& engine) {
        InputActions& actions = engine.GetInputActions();
        for (std::size_t group = 0; group < GroupActions.size(); ++group) {
            g_groupActions[group] = actions.Add(std::string(GroupActions[group]), "Vaisseau", GroupKeys[group]);
        }
        engine.AddGameSystem(Engine::GameStage::AfterPhysics, FireSystem, Fire);
        engine.AddGameSystem(Engine::GameStage::Hud, HudSystem, DrawLock);
        engine.AddGameSystem(Engine::GameStage::Reset, ResetSystem, Reset);
    }

    void UnregisterPlayerWeapons(Engine& engine) {
        for (const std::string_view action : GroupActions) {
            engine.GetInputActions().Remove(action);
        }
        engine.RemoveGameSystem(FireSystem);
        engine.RemoveGameSystem(HudSystem);
        engine.RemoveGameSystem(ResetSystem);
        Reset(engine, 0.0);
        FlightInfo::SetExtra("Armes", {});
    }
}
