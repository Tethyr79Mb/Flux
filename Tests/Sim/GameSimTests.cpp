// Le jeu simulé (GameSim), sans moteur ni rendu, comme sur le serveur : mêmes bits d'actions que le client, systèmes
// qui tournent sans joueur, état propre à chaque simulation et rendu au retrait.
#include "Check.hpp"

#include "GameSim.hpp"

#include "Flux/Core/Simulation.hpp"
#include "Flux/Scene/Scene.hpp"

using namespace Flux;

bool RunGameSimTests() {
    Simulation first;
    std::shared_ptr<Game::SimState> state = Game::RegisterSimulation(first);
    // Les actions du jeu suivent celles du personnage, dans l'ordre de leur déclaration.
    CHECK(state->pilot.forward == first.GetActions().GetId(CharacterActions::Forward));
    CHECK(state->pilot.brake == 7);
    CHECK(state->navigation.warpAction != NoInputAction && state->weapons.groupActions[9] != NoInputAction);

    // Une seconde simulation (une autre zone) a son propre état et les mêmes bits.
    Simulation second;
    const std::shared_ptr<Game::SimState> other = Game::RegisterSimulation(second);
    CHECK(other != state && other->pilot.level == state->pilot.level);

    // Sans joueur (le serveur avant toute connexion) : les systèmes tournent sans rien casser.
    const Entity cube = first.GetScene().CreateEntity("Cube");
    first.Start();
    for (int step = 0; step < 120; ++step) {
        first.Step(SimulationClock::StepSeconds);
    }
    CHECK(first.GetScene().IsValid(cube) && state->notices.empty() && state->navigation.destinations.empty());

    // Objectif de mission oublié avec la scène.
    Game::Navigation::SetWarpObjective(state->navigation, "Mission : test", Math::WorldPosition{});
    first.SceneReplaced();
    CHECK(!state->navigation.objective && state->navigation.objectiveName.empty());

    // Retirés : les systèmes ne gardent plus l'état.
    Game::UnregisterSimulation(first);
    CHECK(state.use_count() == 1);
    return true;
}
