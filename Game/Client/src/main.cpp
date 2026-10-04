#include <Flux/Core/Engine.hpp>
#include <Flux/Core/GameModule.hpp>
#include <Flux/Core/Project.hpp>
#include <Flux/Platform/Console.hpp>
#include <Flux/Platform/Filesystem.hpp>
#include <Flux/Platform/Window.hpp>
#include <Flux/Scene/SceneSerializer.hpp>

#include <cstdio>

int main() {
    Flux::Platform::UseUtf8Console();

    // Le projet est copié à côté de l'exe par le build : ses chemins relatifs pointent vers bin/assets.
    auto project = Flux::ProjectFile::Load(Flux::Platform::GetExecutableDirectory() / "Game.fluxproject");
    if (!project) {
        std::fprintf(stderr, "Projet : %s\n", project.error().c_str());
        return 1;
    }

    Flux::Engine engine({.title = project->name, .freeCamera = false});
    engine.SetProject(*project);

    auto game = Flux::GameModule::Load(Flux::Platform::GetExecutableDirectory() / FLUX_GAME_MODULE_FILE,
                                       Flux::GameModule::LoadMode::Direct);
    if (!game) {
        std::fprintf(stderr, "Module de jeu : %s\n", game.error().c_str());
        return 1;
    }
    game->Register(engine); // enregistre les composants du jeu : la scène de démarrage peut les utiliser

    if (auto loaded = Flux::SceneSerializer::Load(engine.GetScene(), engine.GetComponents(), project->GetStartupScenePath());
        !loaded) {
        std::fprintf(stderr, "Scène de démarrage : %s\n", loaded.error().c_str());
        return 1;
    }
    return engine.Run();
}
