#pragma once

// Commande du joueur devant cet écran (PlayerCommand), d'après son clavier, sa souris et sa caméra : la seule lecture
// des touches pendant un pas de simulation. Côté client seulement : un serveur reçoit les commandes déjà faites.

#include "Flux/Export.hpp"
#include "Flux/Math/Vector3.hpp"
#include "Flux/Math/WorldPosition.hpp"
#include "Flux/Scene/PlayerCommand.hpp"

#include <bitset>
#include <cstdint>

namespace Flux {
    class Input;
    class InputActions;

    // Ce que la caméra du joueur (PlayerCamera, Camera) donne à la commande.
    struct LocalView
    {
        Math::Vector3f walkForward{}; // avant et droite de la caméra, à l'horizontale de l'endroit
        Math::Vector3f walkRight{};
        float stickYaw = 0.0f;
        float stickPitch = 0.0f;
        Math::WorldPosition position{};
        Math::Vector3f forward{0.0f, 0.0f, -1.0f};
    };

    // Actions de la marche, combinées à LocalView::walkForward et walkRight en PlayerCommand::move.
    struct WalkActions
    {
        InputActionId forward = NoInputAction;
        InputActionId back = NoInputAction;
        InputActionId left = NoInputAction;
        InputActionId right = NoInputAction;
    };

    // Commande ajoutée à celle du clavier et de la souris pendant quelques pas, puis relâchée seule (Engine::InjectCommand :
    // l'outil « input » de l'éditeur pilote sans personne aux commandes).
    struct InjectedCommand
    {
        std::bitset<MaxInputActions> held;    // tenues pendant steps pas, enfoncées au premier
        std::bitset<MaxInputActions> pressed; // enfoncées au premier pas seulement (un appui)
        bool steer = false;                   // le manche ci-dessous remplace celui de la souris
        float stickYaw = 0.0f;
        float stickPitch = 0.0f;
        int steps = 0;      // pas restants
        bool first = true;  // prochain pas : le premier
    };

    // keyboardFree faux (saisie dans un champ, touche en cours de réassignation) : aucune action, la vue reste.
    // injected : ajoutée même clavier pris, et un de ses pas consommé. Exportée pour les tests seulement.
    FLUX_API PlayerCommand SampleLocalCommand(const Input& input, const InputActions& actions, bool keyboardFree, const LocalView& view,
                                              const WalkActions& walk, std::uint64_t step,
                                              InjectedCommand* injected = nullptr);
}
