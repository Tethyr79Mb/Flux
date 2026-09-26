#include <Flux/Core/Engine.hpp>
#include <Flux/Core/GameModule.hpp>
#include <Flux/Core/Project.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Filesystem.hpp>

#include "ContentBrowser.hpp"
#include "SceneDocument.hpp"
#include "SceneEditor.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

namespace {
    // FluxEditor.exe [projet.fluxproject] : sans argument, le projet du dépôt (à défaut, celui copié dans bin/).
    std::filesystem::path ProjectPath(int argc, char** argv) {
        if (argc > 1) {
            return argv[1];
        }
        const std::filesystem::path source(u8"" FLUX_DEFAULT_PROJECT);
        std::error_code error;
        return std::filesystem::exists(source, error) ? source
                                                      : Flux::Platform::GetExecutableDirectory() / "Game.fluxproject";
    }
}

int main(int argc, char** argv) {
    // Déclarés avant le moteur, donc détruits après lui : les fonctions qui les référencent ne leur survivent pas.
    FluxEditor::SceneDocument document;
    FluxEditor::SceneEditor sceneEditor;
    FluxEditor::ContentBrowser contentBrowser;
    Flux::Engine engine({.title = "Flux Editor", .windowSize = {1600, 900}});
    engine.SetEditorUpdate([&](Flux::Engine& e, double deltaSeconds) {
        document.Update(e); // barre de menus d'abord : « Ouvrir » remplace la scène avant que les panneaux la décrivent
        sceneEditor.Update(e);
        contentBrowser.Update(e, document, deltaSeconds);
        if (sceneEditor.TakeModified()) {
            document.MarkModified();
        }
    });
    engine.SetCloseRequestHandler([&](Flux::Engine& e) { return document.ConfirmClose(e); });

    // Sans projet, l'éditeur démarre quand même, sur une scène vide : on peut toujours ouvrir un fichier.
    auto project = Flux::ProjectFile::Load(ProjectPath(argc, argv));
    if (project) {
        engine.SetProject(*project);
    } else {
        Flux::Platform::ShowError(engine.GetWindow(), "Projet", project.error());
    }

    // Copie fantôme : la DLL d'origine reste libre pour être recompilée pendant que l'éditeur tourne.
    auto game = Flux::GameModule::Load(Flux::Platform::GetExecutableDirectory() / FLUX_GAME_MODULE_FILE,
                                       Flux::GameModule::LoadMode::ShadowCopy);
    if (!game) {
        std::fprintf(stderr, "Module de jeu : %s\n", game.error().c_str());
        return 1;
    }
    game->Register(engine); // composants du jeu d'abord : la scène de démarrage peut les utiliser
    if (project) {
        document.Load(engine, project->GetStartupScenePath());
    }

    return engine.Run();
}
