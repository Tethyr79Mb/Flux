// Simulation sans rendu (Core/Simulation) : ordre des étapes d'un pas, projectiles qui avancent, et tout oublié à
// l'arrêt ou au remplacement de la scène. C'est ce que fera tourner le serveur de jeu.
#include "Check.hpp"

#include "Flux/Core/Simulation.hpp"
#include "Flux/Physics/PhysicsWorld.hpp"

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
        PlayerCommand command;
        command.pressed[interact] = true;
        simulation.SetLocalCommand(command); // sans personnage : la commande du vol libre
        int taken = 0;
        simulation.AddSystem(Simulation::Stage::Interaction, "premier", [&](Simulation& s, double) { taken += s.ConsumeInteraction() ? 1 : 0; });
        simulation.AddSystem(Simulation::Stage::Interaction, "second", [&](Simulation& s, double) { taken += s.ConsumeInteraction() ? 10 : 0; });
        simulation.Start();
        simulation.Step(SimulationClock::StepSeconds);
        CHECK(taken == 1);
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
    return StagesInOrder() && ProjectilesMoveAndAreForgotten() && InteractionTakenOnce() && ActionsKeepTheirBits();
}
