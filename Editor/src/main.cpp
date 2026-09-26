#include <Flux/Core/Engine.hpp>
#include <Flux/Core/GameModule.hpp>
#include <Flux/Core/Project.hpp>
#include <Flux/Platform/Dialogs.hpp>
#include <Flux/Platform/Filesystem.hpp>

#include <Flux/UI/Context.hpp>

#include "CameraPanel.hpp"
#include "ContentBrowser.hpp"
#include "EditorLayout.hpp"
#include "PlayMode.hpp"
#include "SceneDocument.hpp"
#include "SceneEditor.hpp"
#include "SettingsWindow.hpp"

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
    FluxEditor::PlayMode playMode;
    FluxEditor::EditorLayout layout;
    FluxEditor::SettingsWindow settings;
    // Pas de panneau « Moteur » (images/s, GPU...) : l'éditeur a son panneau Caméra et Fichier > Paramètres.
    Flux::Engine engine({.title = "Flux Editor", .windowSize = {1600, 900}, .maximized = true, .showDebugOverlay = false});
    engine.SetGameState(Flux::GameState::Stopped); // on édite : le jeu ne tourne qu'avec « Jouer »
    engine.SetEditorUpdate([&](Flux::Engine& e, double deltaSeconds) {
        // Barre de menus d'abord : « Ouvrir » ou « Arrêter » remplacent la scène avant que les panneaux la décrivent.
        playMode.HandleShortcuts(e);
        document.SetLocked(playMode.IsPlaying());
        document.HandleShortcuts(e);
        Flux::UI::Context& ui = e.GetUi();
        if (ui.BeginMainMenuBar()) {
            if (ui.BeginMenu("Fichier")) {
                document.DrawSceneMenuItems(ui, e);
                ui.Separator();
                if (ui.MenuItem("Paramètres...")) {
                    settings.Open();
                }
                ui.Separator();
                document.DrawQuitMenuItem(ui, e);
                ui.EndMenu();
            }
            layout.DrawWindowMenu(ui);
            playMode.DrawToolbar(ui, e);
            ui.EndMainMenuBar();
        }
        document.SetLocked(playMode.IsPlaying());

        layout.Update(e);
        sceneEditor.Update(e);
        contentBrowser.Update(e, document, deltaSeconds);
        FluxEditor::DrawCameraPanel(e);
        settings.Draw(e);
        // Pendant le jeu, les modifications disparaissent à l'arrêt : le fichier n'est pas concerné.
        if (sceneEditor.TakeModified() && !playMode.IsPlaying()) {
            document.MarkModified();
        }
        document.UpdateTitle(e);
    });
    engine.SetCloseRequestHandler([&](Flux::Engine& e) {
        playMode.Stop(e); // la question porte sur la scène éditée, pas sur sa copie en jeu
        return document.ConfirmClose(e);
    });

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

    const int exitCode = engine.Run();
    layout.Save(engine);
    return exitCode;
}
