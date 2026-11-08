#include "Game.hpp"

#include "FlightInfo.hpp"
#include "GameSim.hpp"
#include "Hud.hpp"
#include "PlaceItems.hpp"
#include "ServerConnection.hpp"

#include <Flux/Core/Engine.hpp>
#include <Flux/Core/Log.hpp>
#include <Flux/Input/InputActions.hpp>
#include <Flux/Scene/Scene.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <utility>

// Le jeu sur le client : la simulation du jeu (GameSim), et ce qu'en voit et touche le joueur (touches, affichages,
// interfaces, objets de l'éditeur). La scène de démarrage est choisie par le projet (Game.fluxproject).
namespace {
    using namespace Flux;
    using namespace Game;

    // Environ une année-lumière (9,46e15 m) en secteurs de 1 million de km : la grille lointaine de la démo.
    constexpr std::int64_t FarSectorZ = -9'460'730;
    constexpr const char* JumpAction = "Saut d'une année-lumière";
    constexpr const char* SimulationInfoSystem = "Jeu : infos de la simulation";

    // Touches par défaut des actions de la simulation du jeu (déclarées par GameSim, même bit sur le serveur).
    void BindKeys(InputActions& actions) {
        // Comme Star Citizen : Espace monte, Ctrl descend, X freine ; Maj boost et Tab mode voyage, comme X4.
        actions.Add(std::string(Piloting::Brake), "Vaisseau", Key::X);
        actions.Add(std::string(Piloting::Boost), "Vaisseau", Key::LeftShift);
        actions.Add(std::string(Piloting::Travel), "Vaisseau", Key::Tab);
        actions.Add(std::string(Piloting::Up), "Vaisseau", Key::Space);
        actions.Add(std::string(Piloting::Down), "Vaisseau", Key::LeftControl);
        actions.Add(std::string(Piloting::RollLeft), "Vaisseau", Key::Q);
        actions.Add(std::string(Piloting::RollRight), "Vaisseau", Key::E);
        actions.Add(std::string(Piloting::Level), "Vaisseau", Key::H);
        // Près d'une station : se faire conduire au quai libre le plus proche ; amarré : repartir (choix de Bruno).
        actions.Add(std::string(Navigation::AutopilotAction), "Vaisseau", Key::L);
        // Vers la destination que vise le nez (station, planète, objectif de mission) ; en warp : le couper.
        actions.Add(std::string(Navigation::WarpAction), "Vaisseau", Key::J);
        constexpr std::array GroupKeys{Key::Digit1, Key::Digit2, Key::Digit3, Key::Digit4, Key::Digit5,
                                       Key::Digit6, Key::Digit7, Key::Digit8, Key::Digit9, Key::Digit0};
        for (std::size_t group = 0; group < GroupKeys.size(); ++group) {
            actions.Add(std::string(PlayerWeapons::GroupActions[group]), "Vaisseau", GroupKeys[group]);
        }
    }

    void UnbindKeys(InputActions& actions) {
        for (const std::string_view name : Piloting::ShipActions) {
            actions.Remove(name);
        }
        actions.Remove(Navigation::AutopilotAction);
        actions.Remove(Navigation::WarpAction);
        for (const std::string_view name : PlayerWeapons::GroupActions) {
            actions.Remove(name);
        }
        actions.Remove(JumpAction);
    }

    // Lignes et messages que la simulation laisse pour le joueur, passés aux infos de vol avant leur dessin.
    void ShowSimulationInfo(SimState& state) {
        FlightInfo::SetExtra("Navigation", state.navigation.info);
        FlightInfo::SetExtra("Armes", state.weapons.info);
        for (std::string& notice : state.notices) {
            FlightInfo::ShowMessage(std::move(notice));
        }
        state.notices.clear();
    }
}

namespace Game {
    void Register(Engine& engine) {
        const std::shared_ptr<SimState> state = RegisterSimulation(engine.GetSimulation());
        BindKeys(engine.GetInputActions());
        // Saut de la caméra (démo) : propre à cet écran, son bit est pris en haut de la commande (InputActions).
        const InputActionId jump = engine.GetInputActions().Add(JumpAction, "Démo", Key::T);
        engine.SetGameUpdate([jump](Engine& e, double) {
            if (e.GetPlayerCommand(e.GetPlayer()).WasPressed(jump)) {
                Camera& camera = e.GetCamera(); // même point de vue, un autre secteur
                camera.position.sector.z = camera.position.sector.z == 0 ? FarSectorZ : 0;
            }
        });
        engine.AddGameSystem(Engine::GameStage::Hud, SimulationInfoSystem, [state](Engine&, double) { ShowSimulationInfo(*state); });
        RegisterFlightInfo(engine); // avant les autres affichages du jeu : dessinées dessous
        RegisterNavigationHud(engine, state);
        RegisterMissionsHud(engine, state);
        RegisterPlayerWeaponsHud(engine);
        RegisterServerConnection(engine);
        RegisterPlaceItems(engine.GetEditorExtensions()); // retirés par le moteur au déchargement
        RegisterWeaponMount(engine.GetEditorExtensions());
        // Exemple d'action « Fonction C++ » d'un bouton d'interface (cible : « Exemple »).
        engine.RegisterInterfaceFunction("Exemple", [](Engine&) { Log::Info("bouton d'interface : fonction Exemple appelée"); });
        // Exemple de liaison : un curseur lié à « Vitesse de la démo » la change, un texte ou une barre l'affiche.
        engine.BindInterfaceValue(
            "Vitesse de la démo", [state] { return static_cast<double>(state->spinSpeed); },
            [state](double value) { state->spinSpeed = static_cast<float>(value); });
    }

    void Unregister(Engine& engine) {
        engine.SetGameUpdate(nullptr);
        UnregisterSimulation(engine.GetSimulation());
        engine.RemoveGameSystem(SimulationInfoSystem);
        UnregisterNavigationHud(engine);
        UnregisterMissionsHud(engine);
        UnregisterPlayerWeaponsHud(engine);
        UnregisterServerConnection(engine);
        UnregisterFlightInfo(engine);
        engine.UnregisterInterfaceFunction("Exemple");
        engine.UnbindInterfaceValue("Vitesse de la démo");
        UnbindKeys(engine.GetInputActions());
    }
}
