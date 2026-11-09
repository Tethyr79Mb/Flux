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
    CHECK(state->navigation.warp != NoInputAction && state->weapons.groupActions[9] != NoInputAction);

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
    CHECK(first.GetScene().IsValid(cube));

    // Deux joueurs : chacun son état de jeu, sur son personnage.
    Scene& scene = first.GetScene();
    const Entity one = scene.CreateEntity("Un");
    const Entity two = scene.CreateEntity("Deux");
    scene.Add<CharacterComponent>(one);
    scene.Add<CharacterComponent>(two);
    first.Step(SimulationClock::StepSeconds);
    Game::PlayerStateComponent* oneState = Game::FindPlayerState(scene, one);
    CHECK(oneState && Game::FindPlayerState(scene, two) && oneState != Game::FindPlayerState(scene, two));
    Game::Navigation::SetWarpObjective(oneState->navigation, "Mission : test", Math::WorldPosition{});
    CHECK(!Game::FindPlayerState(scene, two)->navigation.objective);
    // Un serveur n'affiche rien : les messages d'un joueur ne s'entassent pas.
    for (int notice = 0; notice < 100; ++notice) {
        Game::AddNotice(*oneState, "message");
    }
    CHECK(oneState->notices.size() == Game::PlayerStateComponent::MaxNotices);
    CHECK(!Game::Missions::Accept(first, one, NullEntity)); // pas de donneur

    // Oubliés à l'arrêt.
    first.Stop();
    CHECK(!Game::FindPlayerState(scene, one));
    first.Start();

    // Retirés : les systèmes ne gardent plus l'état.
    Game::UnregisterSimulation(first);
    CHECK(state.use_count() == 1);
    return true;
}
