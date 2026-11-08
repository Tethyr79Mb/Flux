#pragma once

#include "Flux/Math/Vector3.hpp"
#include "Flux/Math/WorldPosition.hpp"
#include "Flux/SimExport.hpp"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace Flux {
    // Identifiant d'une action : sa place dans les bits de PlayerCommand. Celles que lit la simulation sont déclarées
    // par elle et par le code du jeu qui y tourne (CommandActions), dans le même ordre sur le client et le serveur : ils
    // ont donc les mêmes. Celles du seul client (vue, sauvegarde rapide) prennent les bits du haut (InputActions).
    using InputActionId = std::uint8_t;
    inline constexpr InputActionId NoInputAction = 0xFF;
    inline constexpr std::size_t MaxInputActions = 128;

    // Ce que veut un joueur pendant un pas de simulation : la seule chose que les systèmes du jeu savent de son clavier,
    // de sa souris et de sa caméra. Sur le client, le moteur la remplit à chaque pas ; plus tard, le serveur la recevra
    // du réseau et la simulation n'y verra aucune différence. Champs fixes pour ce qui est continu (marche, manche,
    // vue), un bit par action pour le reste, comme le CUserCmd de Source.
    struct PlayerCommand
    {
        std::uint64_t step = 0; // pas de simulation où elle s'applique (SimulationClock::GetStep)
        // Marche du personnage dans le plan du sol, repère du monde, longueur 0 à 1 (CharacterInput::move).
        Math::Vector3f move{};
        // Manche du pilote (ShipInput::stickYaw et stickPitch) : -1 à 1.
        float stickYaw = 0.0f;
        float stickPitch = 0.0f;
        // D'où regarde le joueur et vers où (unitaire, monde) : visée, verrouillage d'une cible.
        Math::WorldPosition viewPosition{};
        Math::Vector3f viewForward{0.0f, 0.0f, -1.0f};
        // Actions tenues pendant ce pas, et enfoncées depuis le pas précédent.
        std::bitset<MaxInputActions> held;
        std::bitset<MaxInputActions> pressed;

        bool IsDown(InputActionId action) const { return action < MaxInputActions && held[action]; }
        bool WasPressed(InputActionId action) const { return action < MaxInputActions && pressed[action]; }
        // +1 si positive est tenue, -1 si negative l'est, 0 pour les deux ou aucune.
        float GetAxis(InputActionId positive, InputActionId negative) const {
            return (IsDown(positive) ? 1.0f : 0.0f) - (IsDown(negative) ? 1.0f : 0.0f);
        }
    };

    // Actions lues par la simulation (marcher, tirer, warp...), par nom : leur bit dans PlayerCommand. Un nom garde son
    // bit tant que le programme tourne, même retiré puis redéclaré (module de jeu rechargé). Les touches qui les
    // déclenchent sont au client (InputActions).
    class FLUX_SIM_API CommandActions {
        public:
        // Rend le bit de l'action, attribué à la première déclaration de son nom (NoInputAction au-delà de
        // MaxInputActions).
        InputActionId Declare(std::string_view name);
        InputActionId GetId(std::string_view name) const;
        std::size_t GetCount() const { return m_ids.size(); }

        private:
        std::map<std::string, InputActionId, std::less<>> m_ids;
    };
}
