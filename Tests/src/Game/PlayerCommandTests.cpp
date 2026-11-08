// Commandes du joueur (PlayerCommand) : touches lues une fois par pas, identifiants d'actions stables, et un vaisseau
// piloté seulement par des commandes, comme il le sera sur le serveur.
#include "../Check.hpp"

#include "Core/LocalPlayerCommand.hpp"
#include "Piloting.hpp"

#include "Flux/Core/Simulation.hpp"
#include "Flux/Input/InputActions.hpp"
#include "Flux/Physics/PhysicsWorld.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/Ships.hpp"

#include <cmath>

using namespace Flux;
using namespace Game;

namespace {
    constexpr double Frame = 1.0 / 60.0;

    void SetKey(Input& input, Key key, bool down) { input.SetKey(static_cast<std::uint16_t>(key), down); }

    // Actions de marche du moteur (Engine.cpp), déclarées avant celles du jeu comme dans le jeu.
    WalkActions DeclareWalk(InputActions& actions) {
        return {actions.Add("Avancer", "Déplacement", Key::W, Key::Up), actions.Add("Reculer", "Déplacement", Key::S, Key::Down),
                actions.Add("Aller à gauche", "Déplacement", Key::A, Key::Left),
                actions.Add("Aller à droite", "Déplacement", Key::D, Key::Right)};
    }
}

bool RunPlayerCommandTests() {
    // Bits des actions : ceux de la simulation (personnage, puis jeu) d'abord, les mêmes sur le serveur ; les
    // touches du client s'y rattachent par le nom.
    Simulation simulation;
    const PilotActions pilot = Piloting::DeclareActions(simulation.GetActions());
    InputActions actions(&simulation.GetActions());
    const WalkActions walk = DeclareWalk(actions);
    actions.Add(std::string(Piloting::Brake), "Vaisseau", Key::X);
    actions.Add(std::string(Piloting::Travel), "Vaisseau", Key::Tab);
    CHECK(walk.forward == 0 && walk.right == 3);
    CHECK(pilot.forward == walk.forward && pilot.right == walk.right);
    CHECK(pilot.brake != NoInputAction && pilot.level != NoInputAction && pilot.brake != pilot.level);
    CHECK(actions.GetId(Piloting::Brake) == pilot.brake && actions.GetId(Piloting::Travel) == pilot.travel);
    // Action du client seul (vue, sauvegarde) : un bit du haut, qui ne décale pas ceux de la simulation.
    const InputActionId view = actions.Add("Changer de vue", "Vaisseau", Key::V);
    CHECK(view == MaxInputActions - 1);
    CHECK(Piloting::DeclareActions(simulation.GetActions()).level == pilot.level);

    // Module de jeu rechargé : ses actions retirées puis redéclarées gardent leurs bits.
    actions.Remove(Piloting::Brake);
    CHECK(actions.Find(Piloting::Brake) == nullptr);
    CHECK(actions.GetId(Piloting::Brake) == pilot.brake);
    CHECK(actions.Add(std::string(Piloting::Brake), "Vaisseau", Key::X) == pilot.brake);

    // Marche : avant et droite de la vue, selon les touches tenues.
    const LocalView eye{.walkForward = {0.0f, 0.0f, -1.0f}, .walkRight = {1.0f, 0.0f, 0.0f}, .stickYaw = 0.5f};
    Input input;
    input.BeginFrame();
    SetKey(input, Key::W, true);
    SetKey(input, Key::D, true);
    PlayerCommand command = SampleLocalCommand(input, actions, true, eye, walk, 7);
    CHECK(command.step == 7 && command.stickYaw == 0.5f);
    CHECK(command.move == (Math::Vector3f{1.0f, 0.0f, -1.0f}));
    CHECK(command.IsDown(walk.forward) && command.WasPressed(walk.forward) && !command.IsDown(walk.back));
    // Clavier pris (saisie dans un champ) : aucune action, la vue reste.
    command = SampleLocalCommand(input, actions, false, eye, walk, 8);
    CHECK(command.held.none() && command.pressed.none() && command.move == Math::Vector3f{} && command.stickYaw == 0.5f);
    SetKey(input, Key::D, false);

    // Un vaisseau piloté par commandes seulement : 2 s de poussée à 10 m/s² (vitesse de croisière 20 m/s), mode
    // voyage demandé une seule fois par un appui, puis frein jusqu'à l'arrêt.
    Scene scene;
    const Entity ship = scene.CreateEntity("Vaisseau");
    scene.Add<ShipComponent>(ship).fallsWithoutPilot = false;
    PhysicsWorld world;
    const auto steps = [&](double seconds) {
        ShipInput last;
        for (int step = 0; step < static_cast<int>(std::lround(seconds / Frame)); ++step) {
            last = Piloting::ReadShipInput(SampleLocalCommand(input, actions, true, eye, walk, static_cast<std::uint64_t>(step)), pilot);
            input.BeginFrame(); // les appuis ne servent qu'au premier pas
            Ships::Update(scene, ship, Frame, last);
            world.Update(scene, Frame, {});
        }
        return last;
    };
    input.BeginFrame();
    ShipInput read = steps(2.0);
    CHECK(read.thrust == 1.0f && read.strafe == 0.0f && !read.brake && !read.travel);
    const ShipComponent& component = scene.Get<ShipComponent>(ship);
    CHECK(std::abs(component.velocity.z + 20.0f) < 0.5f && std::abs(component.cruiseSpeed - 20.0f) < 0.5f);

    input.BeginFrame();
    SetKey(input, Key::Tab, true);
    PlayerCommand pressed = SampleLocalCommand(input, actions, true, eye, walk, 0);
    CHECK(Piloting::ReadShipInput(pressed, pilot).travel);
    input.BeginFrame();
    PlayerCommand held = SampleLocalCommand(input, actions, true, eye, walk, 1);
    CHECK(held.IsDown(pilot.travel) && !Piloting::ReadShipInput(held, pilot).travel);
    SetKey(input, Key::Tab, false);

    SetKey(input, Key::W, false);
    SetKey(input, Key::X, true);
    input.BeginFrame();
    read = steps(4.0);
    CHECK(read.brake && read.thrust == 0.0f);
    CHECK(Math::Length(component.velocity) < 0.1f);
    return true;
}
