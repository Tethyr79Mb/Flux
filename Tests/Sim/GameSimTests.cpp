// Le jeu simulé (GameSim), sans moteur ni rendu, comme sur le serveur : mêmes bits d'actions que le client, systèmes
// qui tournent sans joueur, état propre à chaque simulation et rendu au retrait.
#include "Check.hpp"

#include "GameSim.hpp"
#include "SpawnPoint.hpp"

#include "Flux/Core/Simulation.hpp"
#include "Flux/Scene/Scene.hpp"
#include "Flux/Scene/SceneSerializer.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>

using namespace Flux;

namespace {
    // Arrivée d'un joueur sur le serveur : personnage du solo retiré, prefab créé au point d'apparition, dans la
    // station qui le porte.
    bool SpawnPoints() {
        Simulation simulation;
        const std::shared_ptr<Game::SimState> state = Game::RegisterSimulation(simulation);
        const std::filesystem::path assets = std::filesystem::temp_directory_path() / "FluxSpawnTests";
        std::filesystem::create_directories(assets / "Prefabs");
        {
            Scene source;
            const Entity hero = source.CreateEntity("Héros");
            source.Add<CharacterComponent>(hero).player = false;
            const auto prefab = SceneSerializer::SavePrefab(source, simulation.GetComponents(), hero,
                                                            SceneSerializer::PrefabIds::ForInstance);
            CHECK(prefab.has_value());
            std::ofstream(assets / "Prefabs" / "Joueur.prefab.json") << *prefab;
        }
        simulation.SetProject({.name = "test", .file = {}, .assetsDirectory = assets, .startupScene = {}});

        Scene& scene = simulation.GetScene();
        CHECK(!Game::SpawnPoints::Spawn(simulation, "", "Bruno")); // pas de point d'apparition
        const Entity station = scene.CreateEntity("Station");
        scene.Add<ShipComponent>(station);
        const Entity hangar = scene.CreateEntity("Hangar", station);
        scene.Get<TransformComponent>(hangar).position = {100.0, 0.0, 0.0};
        const Entity point = scene.CreateEntity("Apparition", hangar);
        scene.Get<TransformComponent>(point).position = {5.0, 1.0, -2.0};
        scene.Add<Game::SpawnPointComponent>(point).prefab = "Prefabs/Joueur.prefab.json";
        const Entity solo = scene.CreateEntity("Joueur du solo", station);
        scene.Add<CharacterComponent>(solo);

        CHECK(Game::SpawnPoints::RemovePlacedPlayers(scene) == 1 && !scene.IsValid(solo));
        // Un prefab qu'aucun point ne propose : celui du point (un client ne crée pas ce qu'il veut).
        const auto spawned = Game::SpawnPoints::Spawn(simulation, "Prefabs/Autre.prefab.json", "Bruno");
        CHECK(spawned.has_value());
        const Entity character = *spawned;
        CHECK(scene.GetParent(character) == station); // enfant direct de la station : il y marche
        CHECK(scene.Get<CharacterComponent>(character).player);
        CHECK(scene.Get<NameComponent>(character).name == "Joueur : Bruno");
        const Math::Vector3d gap = Math::Delta(scene.GetWorldPosition(point), scene.GetWorldPosition(character));
        CHECK(Math::Length(gap) < 1.0e-6);

        std::filesystem::remove_all(assets);
        Game::UnregisterSimulation(simulation);
        return true;
    }
}

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
    return SpawnPoints();
}
