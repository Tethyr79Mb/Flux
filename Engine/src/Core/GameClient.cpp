#include "Flux/Core/GameClient.hpp"

#include "Flux/Core/Engine.hpp"
#include "Flux/Core/GameModule.hpp"
#include "Flux/Core/Log.hpp"
#include "Flux/Core/Project.hpp"
#include "Flux/Platform/Console.hpp"
#include "Flux/Platform/Filesystem.hpp"

namespace Flux {
    int RunGame(const std::filesystem::path& moduleFile) {
        Platform::UseUtf8Console();
        const std::filesystem::path directory = Platform::GetExecutableDirectory();

        // Le projet est copié à côté de l'exe par le build : ses chemins relatifs pointent vers bin/assets.
        auto project = ProjectFile::Load(directory / "Game.fluxproject");
        if (!project) {
            Log::Error("Projet : {}", project.error());
            return 1;
        }

        Engine engine({.title = project->name, .freeCamera = false});
        engine.SetProject(*project);

        // Déclaré après le moteur : détruit avant lui, le jeu se désenregistre d'un moteur encore entier.
        auto game = GameModule::Load(directory / moduleFile, GameModule::LoadMode::Direct);
        if (!game) {
            Log::Error("Module de jeu : {}", game.error());
            return 1;
        }
        game->Register(engine); // enregistre les composants du jeu : la scène de démarrage peut les utiliser

        if (auto loaded = engine.LoadScene(project->GetStartupScenePath()); !loaded) {
            Log::Error("Scène de démarrage : {}", loaded.error());
            return 1;
        }
        return engine.Run();
    }
}
