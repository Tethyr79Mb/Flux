// Simulation sans rendu (Core/Simulation) : ordre des étapes d'un pas, projectiles qui avancent, et tout oublié à
// l'arrêt ou au remplacement de la scène. C'est ce que fera tourner le serveur de jeu.
#include "Check.hpp"

#include "Flux/Core/Simulation.hpp"
#include "Flux/Physics/PhysicsWorld.hpp"

#include <cmath>
#include <string>

using namespace Flux;

namespace {
    bool StagesInOrder() {
        Simulation simulation;
        std::string order;
        simulation.AddSystem(Simulation::Stage::AfterPhysics, "après", [&](Simulation&, double) { order += 'A'; });
        simulation.AddSystem(Simulation::Stage::Interaction, "interaction", [&](Simulation&, double) { order += 'I'; });
        simulation.AddSystem(Simulation::Stage::BeforePhysics, "avant", [&](Simulation&, double) { order += 'B'; });
        simulation.AddSystem(Simulation::Stage::Reset, "oubli", [&](Simulation&, double) { order += 'R'; });
        simulation.Start();
        CHECK(simulation.GetPhysics() != nullptr);
        simulation.Step(SimulationClock::StepSeconds);
        CHECK(order == "IBA");
        // Remplacé : un seul système du même nom.
        simulation.AddSystem(Simulation::Stage::Interaction, "interaction", [&](Simulation&, double) { order += 'i'; });
        simulation.Step(SimulationClock::StepSeconds);
        CHECK(order == "IBAiBA");
        simulation.RemoveSystem("avant");
        order.clear();
        simulation.Step(SimulationClock::StepSeconds);
        CHECK(order == "iA");
        order.clear();
        simulation.Stop();
        CHECK(order == "R");
        CHECK(simulation.GetPhysics() == nullptr);
        return true;
    }

    bool ProjectilesMoveAndAreForgotten() {
        Simulation simulation;
        simulation.Start();
        Projectile shot;
        shot.velocity = {0.0f, 0.0f, -100.0f};
        shot.life = 10.0f;
        simulation.GetProjectiles().push_back(shot);
        simulation.Step(SimulationClock::StepSeconds);
        CHECK(simulation.GetProjectiles().size() == 1);
        CHECK(simulation.GetProjectiles()[0].position.offset.z < -1.0);
        // Scène remplacée : les tirs de l'ancienne disparaissent, un nouveau monde physique est prêt.
        simulation.SceneReplaced();
        CHECK(simulation.GetProjectiles().empty());
        CHECK(simulation.GetPhysics() != nullptr);
        simulation.Stop();
        CHECK(simulation.GetProjectiles().empty());
        return true;
    }

    bool InteractionTakenOnce() {
        Simulation simulation;
        const InputActionId interact = simulation.GetActions().GetId(CharacterActions::Interact);
        CHECK(interact != NoInputAction);
        const Entity hero = simulation.GetScene().CreateEntity("Joueur");
        simulation.GetScene().Add<CharacterComponent>(hero);
        PlayerCommand command;
        command.pressed[interact] = true;
        simulation.SetLocalCommand(command);
        int taken = 0;
        simulation.AddSystem(Simulation::Stage::Interaction, "premier",
                             [&](Simulation& s, double) { taken += s.ConsumeInteraction(hero) ? 1 : 0; });
        simulation.AddSystem(Simulation::Stage::Interaction, "second",
                             [&](Simulation& s, double) { taken += s.ConsumeInteraction(hero) ? 10 : 0; });
        simulation.Start();
        simulation.Step(SimulationClock::StepSeconds);
        CHECK(taken == 1);
        return true;
    }

    // Deux joueurs dans la même scène (un serveur) : chacun sa commande, son Interagir et sa marche.
    bool TwoPlayers() {
        Simulation simulation;
        Scene& scene = simulation.GetScene();
        const Entity floor = scene.CreateEntity("Sol");
        scene.Get<TransformComponent>(floor).position = {0.0, -0.5, 0.0};
        scene.Get<TransformComponent>(floor).scale = {100.0f, 1.0f, 100.0f};
        scene.Add<ColliderComponent>(floor);
        const Entity first = scene.CreateEntity("Premier");
        const Entity second = scene.CreateEntity("Second");
        scene.Get<TransformComponent>(second).position = {0.0, 0.0, 10.0};
        scene.Add<CharacterComponent>(first);
        scene.Add<CharacterComponent>(second);
        const Entity bystander = scene.CreateEntity("Passant"); // personnage sans joueur : pas dans GetPlayers
        scene.Get<TransformComponent>(bystander).position = {0.0, 0.0, -10.0};
        scene.Add<CharacterComponent>(bystander).player = false;

        const InputActionId interact = simulation.GetActions().GetId(CharacterActions::Interact);
        std::size_t players = 0;
        bool firstInteracts = true;
        bool secondInteracts = false;
        simulation.AddSystem(Simulation::Stage::Interaction, "joueurs", [&](Simulation& s, double) {
            players = s.GetPlayers().size();
            firstInteracts = s.ConsumeInteraction(first);
            secondInteracts = s.ConsumeInteraction(second);
        });
        simulation.Start();
        for (int step = 0; step < 60; ++step) {
            // Le second marche vers +x et appuie sur Interagir ; le premier ne fait rien.
            PlayerCommand command;
            command.move = {1.0f, 0.0f, 0.0f};
            command.pressed[interact] = true;
            simulation.SetPlayerCommand(second, command);
            simulation.Step(SimulationClock::StepSeconds);
            CHECK(players == 2 && !firstInteracts && secondInteracts);
        }
        CHECK(std::abs(scene.Get<TransformComponent>(first).position.x) < 0.01);
        CHECK(scene.Get<TransformComponent>(second).position.x > 1.0);
        // Une commande ne vaut que pour son pas : sans nouvelle commande, il n'appuie plus.
        simulation.Step(SimulationClock::StepSeconds);
        CHECK(!secondInteracts && simulation.GetPlayerCommand(second).move.x == 0.0f);
        return true;
    }

    // Bits des actions : ceux du personnage d'abord, dans l'ordre, puis ceux du jeu ; un nom garde le sien.
    bool ActionsKeepTheirBits() {
        Simulation simulation;
        CommandActions& actions = simulation.GetActions();
        CHECK(actions.GetId(CharacterActions::Forward) == 0 && actions.GetId(CharacterActions::Interact) == 6);
        const InputActionId warp = actions.Declare("Warp");
        CHECK(warp == 7 && actions.Declare("Warp") == warp && actions.GetCount() == 8);
        CHECK(actions.GetId("Inconnue") == NoInputAction);
        return true;
    }
}

bool RunSimulationTests() {
    return StagesInOrder() && ProjectilesMoveAndAreForgotten() && InteractionTakenOnce() && TwoPlayers()
        && ActionsKeepTheirBits();
}
